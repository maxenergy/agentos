#include "core/audit/audit_logger.hpp"

#include "utils/atomic_file.hpp"
#include "utils/secret_redaction.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {
using OrderedJson = nlohmann::ordered_json;
}  // namespace

namespace {

std::string CurrentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm buffer{};
#ifdef _WIN32
    gmtime_s(&buffer, &time);
#else
    gmtime_r(&time, &buffer);
#endif

    std::ostringstream stream;
    stream << std::put_time(&buffer, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

bool IsLikelyJsonObjectLine(const std::string& line) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || line[first] != '{') {
        return false;
    }

    const auto last = line.find_last_not_of(" \t\r\n");
    return last != std::string::npos && line[last] == '}';
}

constexpr char kDelimiter = '\t';

int HexValue(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

std::string DecodeField(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = HexValue(value[index + 1]);
            const auto low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(value[index]);
    }
    return output;
}

std::vector<std::string> SplitLine(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto delimiter = line.find(kDelimiter, start);
        if (delimiter == std::string::npos) {
            parts.push_back(DecodeField(line.substr(start)));
            break;
        }
        parts.push_back(DecodeField(line.substr(start, delimiter - start)));
        start = delimiter + 1;
    }
    return parts;
}

std::string ExtractJsonStringField(const std::string& line, const std::string_view field_name) {
    const std::string marker = "\"" + std::string(field_name) + "\":\"";
    const auto start = line.find(marker);
    if (start == std::string::npos) {
        return {};
    }
    const auto value_start = start + marker.size();
    const auto value_end = line.find('"', value_start);
    if (value_end == std::string::npos) {
        return {};
    }
    return line.substr(value_start, value_end - value_start);
}

std::string ExtractAuditEventName(const std::string& line) {
    return ExtractJsonStringField(line, "event");
}

std::string ExtractAuditTaskId(const std::string& line) {
    return ExtractJsonStringField(line, "task_id");
}

std::string ExtractAuditTimestamp(const std::string& line) {
    return ExtractJsonStringField(line, "ts");
}

bool ShouldPreserveExistingAuditLine(const std::string& line) {
    if (!IsLikelyJsonObjectLine(line)) {
        return false;
    }
    const auto event = ExtractAuditEventName(line);
    return event.empty() ||
           (event != "task_start" &&
            event != "route" &&
            event != "step" &&
            event != "task_end" &&
            event != "scheduler_run");
}

RouteTargetKind ParseRouteTargetKind(const std::string& value) {
    if (value == "skill") {
        return RouteTargetKind::skill;
    }
    if (value == "agent") {
        return RouteTargetKind::agent;
    }
    return RouteTargetKind::none;
}

struct RecoveredTaskRecord {
    std::string task_id;
    std::string task_type;
    std::string objective;
    bool success = false;
    bool from_cache = false;
    int duration_ms = 0;
    RouteTargetKind route_kind = RouteTargetKind::none;
    std::string route_target;
    std::string error_code;
    std::string error_message;
};

struct RecoveredStepRecord {
    RouteTargetKind target_kind = RouteTargetKind::none;
    std::string target_name;
    bool success = false;
    int duration_ms = 0;
    std::string summary;
    std::string error_code;
    std::string error_message;
};

struct RecoveredLifecycleTimestamps {
    std::string task_start_ts;
    std::string route_ts;
    std::vector<std::string> step_timestamps;
    std::string task_end_ts;
};

struct PreservedAuditLine {
    std::string line;
    std::string timestamp;
};

struct RecoveredSchedulerRunRecord {
    std::string schedule_id;
    std::string task_id;
    long long started_epoch_ms = 0;
    long long completed_epoch_ms = 0;
    int run_count = 0;
    bool success = false;
    bool rescheduled = false;
    std::string route_kind;
    std::string route_target;
    std::string error_code;
    std::string error_message;
    int duration_ms = 0;
};

struct OrderedAuditLine {
    std::string line;
    std::string timestamp;
    int fallback_order = 0;
};

struct OrderedAuditChunk {
    std::vector<std::string> lines;
    std::string timestamp;
    int fallback_order = 0;
};

std::string TimestampOrNow(const std::string& timestamp) {
    if (!timestamp.empty()) {
        return timestamp;
    }
    return CurrentTimestamp();
}

std::string TimestampFromEpochMs(const long long epoch_ms) {
    if (epoch_ms <= 0) {
        return CurrentTimestamp();
    }
    const auto seconds = static_cast<std::time_t>(epoch_ms / 1000LL);
    std::tm buffer{};
#ifdef _WIN32
    gmtime_s(&buffer, &seconds);
#else
    gmtime_r(&seconds, &buffer);
#endif
    std::ostringstream stream;
    stream << std::put_time(&buffer, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

int ParseIntField(const std::string& value, const int fallback = 0) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

long long ParseLongLongField(const std::string& value, const long long fallback = 0) {
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

std::vector<RecoveredTaskRecord> LoadRecoveredTasks(const std::filesystem::path& storage_dir) {
    std::vector<RecoveredTaskRecord> tasks;
    std::ifstream input(storage_dir / "task_log.tsv", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 11) {
            continue;
        }
        tasks.push_back(RecoveredTaskRecord{
            .task_id = parts[0],
            .task_type = parts[1],
            .objective = parts[2],
            .success = parts[4] == "1",
            .from_cache = parts[5] == "1",
            .duration_ms = std::stoi(parts[6]),
            .route_kind = ParseRouteTargetKind(parts[7]),
            .route_target = parts[8],
            .error_code = parts[9],
            .error_message = parts[10],
        });
    }
    return tasks;
}

std::vector<RecoveredSchedulerRunRecord> LoadRecoveredSchedulerRuns(const std::filesystem::path& recovery_storage_dir) {
    std::vector<RecoveredSchedulerRunRecord> runs;
    const auto runtime_dir = recovery_storage_dir.filename() == "memory"
        ? recovery_storage_dir.parent_path()
        : recovery_storage_dir;
    std::ifstream input(runtime_dir / "scheduler" / "runs.tsv", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 12) {
            continue;
        }
        runs.push_back(RecoveredSchedulerRunRecord{
            .schedule_id = parts[0],
            .task_id = parts[1],
            .started_epoch_ms = ParseLongLongField(parts[2]),
            .completed_epoch_ms = ParseLongLongField(parts[3]),
            .run_count = ParseIntField(parts[4]),
            .success = parts[5] == "1",
            .rescheduled = parts[6] == "1",
            .route_kind = parts[7],
            .route_target = parts[8],
            .error_code = parts[9],
            .error_message = parts[10],
            .duration_ms = ParseIntField(parts[11]),
        });
    }
    return runs;
}

std::map<std::string, std::vector<RecoveredStepRecord>> LoadRecoveredSteps(const std::filesystem::path& storage_dir) {
    std::map<std::string, std::vector<RecoveredStepRecord>> steps_by_task;
    std::ifstream input(storage_dir / "step_log.tsv", std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 9) {
            continue;
        }
        steps_by_task[parts[0]].push_back(RecoveredStepRecord{
            .target_kind = ParseRouteTargetKind(parts[1]),
            .target_name = parts[2],
            .success = parts[3] == "1",
            .duration_ms = std::stoi(parts[4]),
            .summary = parts[6],
            .error_code = parts[7],
            .error_message = parts[8],
        });
    }
    return steps_by_task;
}

}  // namespace

AuditLogger::AuditLogger(std::filesystem::path log_path)
    : log_path_(std::move(log_path)) {
    std::filesystem::create_directories(log_path_.parent_path());
}

void AuditLogger::record_task_start(const TaskRequest& task) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        sensitive_values_by_task_[task.task_id] = SensitiveValuesFromMap(task.inputs);
    }

    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "task_start";
    event["task_id"] = task.task_id;
    event["task_type"] = task.task_type;
    event["objective"] = redact_for_task(task.task_id, task.objective);
    event["workspace"] = task.workspace_path.string();
    append_line(event.dump());
}

void AuditLogger::record_route(const TaskRequest& task, const RouteDecision& route) {
    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "route";
    event["task_id"] = task.task_id;
    event["target_kind"] = route_target_kind_name(route.target_kind);
    event["target_name"] = route.target_name;
    event["workflow_name"] = route.workflow_name.value_or("");
    event["rationale"] = redact_for_task(task.task_id, route.rationale);
    append_line(event.dump());
}

void AuditLogger::record_policy(
    const std::string& task_id,
    const std::string& target_name,
    const PolicyDecision& decision) {
    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "policy";
    event["task_id"] = task_id;
    event["target_name"] = target_name;
    event["allowed"] = decision.allowed;
    event["reason"] = redact_for_task(task_id, decision.reason);
    append_line(event.dump());
}

void AuditLogger::record_step(const std::string& task_id, const TaskStepRecord& step) {
    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "step";
    event["task_id"] = task_id;
    event["target_kind"] = route_target_kind_name(step.target_kind);
    event["target_name"] = step.target_name;
    event["success"] = step.success;
    event["duration_ms"] = step.duration_ms;
    event["summary"] = redact_for_task(task_id, step.summary);
    event["error_code"] = step.error_code;
    event["error_message"] = redact_for_task(task_id, step.error_message);
    append_line(event.dump());
}

void AuditLogger::record_task_end(const std::string& task_id, const TaskRunResult& result) {
    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "task_end";
    event["task_id"] = task_id;
    event["success"] = result.success;
    event["from_cache"] = result.from_cache;
    event["route_kind"] = route_target_kind_name(result.route_kind);
    event["route_target"] = result.route_target;
    event["duration_ms"] = result.duration_ms;
    event["summary"] = redact_for_task(task_id, result.summary);
    event["error_code"] = result.error_code;
    event["error_message"] = redact_for_task(task_id, result.error_message);
    append_line(event.dump());
}

void AuditLogger::record_config_diagnostic(
    const std::string& source,
    const std::filesystem::path& file,
    const int line_number,
    const std::string& reason) {
    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "config_diagnostic";
    event["source"] = source;
    event["file"] = file.string();
    event["line"] = line_number;
    event["reason"] = reason;
    append_line(event.dump());
}

void AuditLogger::record_trust_event(
    const std::string& action,
    const std::string& identity_id,
    const std::string& device_id,
    const bool success,
    const std::string& message) {
    OrderedJson event;
    event["ts"] = CurrentTimestamp();
    event["event"] = "trust";
    event["action"] = action;
    event["identity_id"] = identity_id;
    event["device_id"] = device_id;
    event["success"] = success;
    event["message"] = message;
    append_line(event.dump());
}

const std::filesystem::path& AuditLogger::log_path() const {
    return log_path_;
}

void AuditLogger::compact_log(const std::filesystem::path& recovery_storage_dir) {
    std::lock_guard<std::mutex> guard(mutex_);

    std::ostringstream output;
    std::vector<PreservedAuditLine> preserved_global_lines;
    std::map<std::string, std::vector<PreservedAuditLine>> preserved_task_lines;
    std::map<std::string, RecoveredLifecycleTimestamps> recovered_timestamps_by_task;
    {
        std::ifstream input(log_path_, std::ios::binary);
        if (input) {
            std::string line;
            while (std::getline(input, line)) {
                const auto keep_line = recovery_storage_dir.empty() ? IsLikelyJsonObjectLine(line) : ShouldPreserveExistingAuditLine(line);
                if (keep_line) {
                    if (recovery_storage_dir.empty()) {
                        output << line << '\n';
                        continue;
                    }

                    const auto task_id = ExtractAuditTaskId(line);
                    if (!task_id.empty()) {
                        preserved_task_lines[task_id].push_back(PreservedAuditLine{
                            .line = line,
                            .timestamp = ExtractAuditTimestamp(line),
                        });
                    } else {
                        preserved_global_lines.push_back(PreservedAuditLine{
                            .line = line,
                            .timestamp = ExtractAuditTimestamp(line),
                        });
                    }
                    continue;
                }

                if (!recovery_storage_dir.empty() && IsLikelyJsonObjectLine(line)) {
                    const auto task_id = ExtractAuditTaskId(line);
                    if (task_id.empty()) {
                        continue;
                    }

                    auto& timestamps = recovered_timestamps_by_task[task_id];
                    const auto event = ExtractAuditEventName(line);
                    const auto timestamp = ExtractAuditTimestamp(line);
                    if (timestamp.empty()) {
                        continue;
                    }

                    if (event == "task_start" && timestamps.task_start_ts.empty()) {
                        timestamps.task_start_ts = timestamp;
                    } else if (event == "route" && timestamps.route_ts.empty()) {
                        timestamps.route_ts = timestamp;
                    } else if (event == "step") {
                        timestamps.step_timestamps.push_back(timestamp);
                    } else if (event == "task_end" && timestamps.task_end_ts.empty()) {
                        timestamps.task_end_ts = timestamp;
                    }
                }
            }
        }
    }

    if (!recovery_storage_dir.empty()) {
        std::vector<OrderedAuditChunk> ordered_chunks;
        std::set<std::string> recovered_task_ids;
        for (std::size_t index = 0; index < preserved_global_lines.size(); ++index) {
            ordered_chunks.push_back(OrderedAuditChunk{
                .lines = {preserved_global_lines[index].line},
                .timestamp = preserved_global_lines[index].timestamp,
                .fallback_order = static_cast<int>(index),
            });
        }

        const auto tasks = LoadRecoveredTasks(recovery_storage_dir);
        const auto steps_by_task = LoadRecoveredSteps(recovery_storage_dir);
        const auto scheduler_runs = LoadRecoveredSchedulerRuns(recovery_storage_dir);
        for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
            const auto& task = tasks[task_index];
            recovered_task_ids.insert(task.task_id);
            const auto timestamp_it = recovered_timestamps_by_task.find(task.task_id);
            const RecoveredLifecycleTimestamps empty_timestamps;
            const auto& timestamps = timestamp_it != recovered_timestamps_by_task.end() ? timestamp_it->second : empty_timestamps;
            OrderedJson task_start_json;
            task_start_json["ts"] = TimestampOrNow(timestamps.task_start_ts);
            task_start_json["event"] = "task_start";
            task_start_json["task_id"] = task.task_id;
            task_start_json["task_type"] = task.task_type;
            task_start_json["objective"] = task.objective;
            task_start_json["workspace"] = "";
            const auto task_start_line = task_start_json.dump();

            OrderedJson route_json;
            route_json["ts"] = TimestampOrNow(timestamps.route_ts);
            route_json["event"] = "route";
            route_json["task_id"] = task.task_id;
            route_json["target_kind"] = route_target_kind_name(task.route_kind);
            route_json["target_name"] = task.route_target;
            route_json["workflow_name"] = "";
            route_json["rationale"] = "recovered from runtime/memory logs";
            const auto route_line = route_json.dump();

            std::vector<OrderedAuditLine> ordered_lines;
            ordered_lines.push_back(OrderedAuditLine{
                .line = task_start_line,
                .timestamp = TimestampOrNow(timestamps.task_start_ts),
                .fallback_order = 0,
            });
            ordered_lines.push_back(OrderedAuditLine{
                .line = route_line,
                .timestamp = TimestampOrNow(timestamps.route_ts),
                .fallback_order = 10,
            });

            std::vector<std::string> untimed_preserved_lines;
            if (const auto preserved = preserved_task_lines.find(task.task_id); preserved != preserved_task_lines.end()) {
                for (std::size_t index = 0; index < preserved->second.size(); ++index) {
                    const auto& preserved_line = preserved->second[index];
                    if (preserved_line.timestamp.empty()) {
                        untimed_preserved_lines.push_back(preserved_line.line);
                        continue;
                    }
                    ordered_lines.push_back(OrderedAuditLine{
                        .line = preserved_line.line,
                        .timestamp = preserved_line.timestamp,
                        .fallback_order = 20 + static_cast<int>(index),
                    });
                }
            }

            if (const auto it = steps_by_task.find(task.task_id); it != steps_by_task.end()) {
                for (std::size_t index = 0; index < it->second.size(); ++index) {
                    const auto& step = it->second[index];
                    const auto step_timestamp = index < timestamps.step_timestamps.size()
                        ? timestamps.step_timestamps[index]
                        : "";
                    OrderedJson step_json;
                    step_json["ts"] = TimestampOrNow(step_timestamp);
                    step_json["event"] = "step";
                    step_json["task_id"] = task.task_id;
                    step_json["target_kind"] = route_target_kind_name(step.target_kind);
                    step_json["target_name"] = step.target_name;
                    step_json["success"] = step.success;
                    step_json["duration_ms"] = step.duration_ms;
                    step_json["summary"] = step.summary;
                    step_json["error_code"] = step.error_code;
                    step_json["error_message"] = step.error_message;
                    ordered_lines.push_back(OrderedAuditLine{
                        .line = step_json.dump(),
                        .timestamp = TimestampOrNow(step_timestamp),
                        .fallback_order = 100 + static_cast<int>(index),
                    });
                }
            }

            OrderedJson task_end_json;
            task_end_json["ts"] = TimestampOrNow(timestamps.task_end_ts);
            task_end_json["event"] = "task_end";
            task_end_json["task_id"] = task.task_id;
            task_end_json["success"] = task.success;
            task_end_json["from_cache"] = task.from_cache;
            task_end_json["route_kind"] = route_target_kind_name(task.route_kind);
            task_end_json["route_target"] = task.route_target;
            task_end_json["duration_ms"] = task.duration_ms;
            task_end_json["summary"] = "recovered from runtime/memory logs";
            task_end_json["error_code"] = task.error_code;
            task_end_json["error_message"] = task.error_message;
            ordered_lines.push_back(OrderedAuditLine{
                .line = task_end_json.dump(),
                .timestamp = TimestampOrNow(timestamps.task_end_ts),
                .fallback_order = 1000,
            });

            std::stable_sort(ordered_lines.begin(), ordered_lines.end(), [](const OrderedAuditLine& left, const OrderedAuditLine& right) {
                if (!left.timestamp.empty() && !right.timestamp.empty() && left.timestamp != right.timestamp) {
                    return left.timestamp < right.timestamp;
                }
                return left.fallback_order < right.fallback_order;
            });

            int untimed_insert_order = 0;
            for (const auto& line : ordered_lines) {
                ordered_chunks.push_back(OrderedAuditChunk{
                    .lines = {line.line},
                    .timestamp = line.timestamp,
                    .fallback_order = 1000 + static_cast<int>(task_index) * 10000 + line.fallback_order,
                });
                if (line.fallback_order == 10) {
                    for (const auto& untimed_line : untimed_preserved_lines) {
                        ordered_chunks.push_back(OrderedAuditChunk{
                            .lines = {untimed_line},
                            .timestamp = line.timestamp,
                            .fallback_order = 1000 + static_cast<int>(task_index) * 10000 + 11 + untimed_insert_order++,
                        });
                    }
                }
            }
        }

        for (std::size_t run_index = 0; run_index < scheduler_runs.size(); ++run_index) {
            const auto& run = scheduler_runs[run_index];
            const auto timestamp = TimestampFromEpochMs(run.completed_epoch_ms > 0 ? run.completed_epoch_ms : run.started_epoch_ms);
            OrderedJson run_json;
            run_json["ts"] = timestamp;
            run_json["event"] = "scheduler_run";
            run_json["schedule_id"] = run.schedule_id;
            run_json["task_id"] = run.task_id;
            run_json["started_epoch_ms"] = run.started_epoch_ms;
            run_json["completed_epoch_ms"] = run.completed_epoch_ms;
            run_json["run_count"] = run.run_count;
            run_json["success"] = run.success;
            run_json["rescheduled"] = run.rescheduled;
            run_json["route_kind"] = run.route_kind;
            run_json["route_target"] = run.route_target;
            run_json["duration_ms"] = run.duration_ms;
            run_json["error_code"] = run.error_code;
            run_json["error_message"] = run.error_message;
            ordered_chunks.push_back(OrderedAuditChunk{
                .lines = {run_json.dump()},
                .timestamp = timestamp,
                .fallback_order = 50000 + static_cast<int>(run_index),
            });
        }

        int orphan_fallback_order = 100000;
        for (const auto& [task_id, lines] : preserved_task_lines) {
            if (recovered_task_ids.contains(task_id)) {
                continue;
            }

            OrderedAuditChunk orphan_chunk{
                .fallback_order = orphan_fallback_order++,
            };
            for (const auto& preserved_line : lines) {
                orphan_chunk.lines.push_back(preserved_line.line);
                if (orphan_chunk.timestamp.empty() && !preserved_line.timestamp.empty()) {
                    orphan_chunk.timestamp = preserved_line.timestamp;
                }
            }
            ordered_chunks.push_back(std::move(orphan_chunk));
        }

        std::stable_sort(ordered_chunks.begin(), ordered_chunks.end(), [](const OrderedAuditChunk& left, const OrderedAuditChunk& right) {
            if (left.timestamp.empty() != right.timestamp.empty()) {
                return !left.timestamp.empty();
            }
            if (!left.timestamp.empty() && left.timestamp != right.timestamp) {
                return left.timestamp < right.timestamp;
            }
            return left.fallback_order < right.fallback_order;
        });

        for (const auto& chunk : ordered_chunks) {
            for (const auto& line : chunk.lines) {
                output << line << '\n';
            }
        }
    }

    std::ofstream rewritten(log_path_, std::ios::binary | std::ios::trunc);
    rewritten << output.str();
    rewritten.flush();
}

std::vector<std::string> AuditLogger::sensitive_values_for_task(const std::string& task_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    const auto it = sensitive_values_by_task_.find(task_id);
    if (it == sensitive_values_by_task_.end()) {
        return {};
    }
    return it->second;
}

std::string AuditLogger::redact_for_task(const std::string& task_id, std::string value) {
    return RedactSensitiveText(std::move(value), sensitive_values_for_task(task_id));
}

void AuditLogger::append_line(const std::string& line) {
    std::lock_guard<std::mutex> guard(mutex_);
    AppendLineToFile(log_path_, line);
}

}  // namespace agentos
