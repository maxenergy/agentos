#include "scheduler/scheduler.hpp"

#include "core/loop/agent_loop.hpp"
#include "scheduler/cron.hpp"
#include "scheduler/timezone.hpp"
#include "utils/atomic_file.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

std::string EncodeField(const std::string& value, const bool encode_pair_delimiters = false) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r' ||
            (encode_pair_delimiters && (ch == '&' || ch == '='))) {
            constexpr char hex[] = "0123456789ABCDEF";
            output << '%' << hex[(ch >> 4) & 0x0F] << hex[ch & 0x0F];
        } else {
            output << static_cast<char>(ch);
        }
    }
    return output.str();
}

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

StringMap ParseInputs(const std::string& value) {
    StringMap inputs;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto delimiter = value.find('&', start);
        const auto item = delimiter == std::string::npos
            ? value.substr(start)
            : value.substr(start, delimiter - start);

        if (!item.empty()) {
            const auto separator = item.find('=');
            if (separator != std::string::npos) {
                inputs[DecodeField(item.substr(0, separator))] = DecodeField(item.substr(separator + 1));
            }
        }

        if (delimiter == std::string::npos) {
            break;
        }
        start = delimiter + 1;
    }

    return inputs;
}

std::string SerializeInputs(const StringMap& inputs) {
    std::map<std::string, std::string> ordered(inputs.begin(), inputs.end());
    std::ostringstream output;
    bool first = true;
    for (const auto& [key, value] : ordered) {
        if (!first) {
            output << '&';
        }
        first = false;
        output << EncodeField(key, true) << '=' << EncodeField(value, true);
    }
    return output.str();
}

std::vector<std::string> ParseStringList(const std::string& value) {
    std::vector<std::string> items;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto delimiter = value.find('&', start);
        const auto item = delimiter == std::string::npos
            ? value.substr(start)
            : value.substr(start, delimiter - start);
        if (!item.empty()) {
            items.push_back(DecodeField(item));
        }
        if (delimiter == std::string::npos) {
            break;
        }
        start = delimiter + 1;
    }
    return items;
}

std::string SerializeStringList(const std::vector<std::string>& items) {
    std::ostringstream output;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
            output << '&';
        }
        output << EncodeField(items[index], true);
    }
    return output.str();
}

int ParseInt(const std::string& value, const int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

long long ParseLongLong(const std::string& value, const long long fallback) {
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

double ParseDouble(const std::string& value, const double fallback) {
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
}

std::string NormalizeMissedRunPolicy(const std::string& value) {
    if (value == "skip") {
        return "skip";
    }
    return "run-once";
}

bool ShouldSkipMissedIntervalRun(const ScheduledTask& scheduled_task, const long long now_epoch_ms) {
    if (scheduled_task.missed_run_policy != "skip" || scheduled_task.interval_seconds <= 0 || !scheduled_task.cron_expression.empty()) {
        return false;
    }

    const auto interval_ms = static_cast<long long>(scheduled_task.interval_seconds) * 1000LL;
    return now_epoch_ms >= scheduled_task.next_run_epoch_ms + interval_ms;
}

Timezone ResolveTimezoneOrUtc(const std::string& name) {
    if (name.empty()) return Timezone::Utc();
    auto parsed = Timezone::Parse(name);
    if (!parsed) return Timezone::Utc();
    return *parsed;
}

std::optional<long long> ComputeNextCronRunEpochMs(const std::string& expression,
                                                    const std::string& timezone_name,
                                                    const long long after_epoch_ms) {
    auto cron = CronExpression::Parse(expression);
    if (!cron) return std::nullopt;
    const Timezone tz = ResolveTimezoneOrUtc(timezone_name);
    const auto after_tp = std::chrono::system_clock::time_point{
        std::chrono::milliseconds{after_epoch_ms}};
    const auto next = cron->next_after(after_tp, tz);
    if (!next) return std::nullopt;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        next->time_since_epoch()).count();
}

bool ShouldSkipMissedCronRun(const ScheduledTask& scheduled_task, const long long now_epoch_ms) {
    if (scheduled_task.missed_run_policy != "skip" || scheduled_task.cron_expression.empty()) {
        return false;
    }
    const auto next_after_original = ComputeNextCronRunEpochMs(
        scheduled_task.cron_expression, scheduled_task.timezone_name,
        scheduled_task.next_run_epoch_ms);
    return next_after_original.has_value() && now_epoch_ms >= *next_after_original;
}

}  // namespace

Scheduler::Scheduler(std::filesystem::path store_path)
    : store_path_(std::move(store_path)),
      history_path_(store_path_.parent_path() / "runs.tsv") {
    if (!store_path_.parent_path().empty()) {
        std::filesystem::create_directories(store_path_.parent_path());
    }
    load();
}

ScheduledTask Scheduler::save(ScheduledTask scheduled_task) {
    remove(scheduled_task.schedule_id);
    scheduled_task.missed_run_policy = NormalizeMissedRunPolicy(scheduled_task.missed_run_policy);
    if (!scheduled_task.timezone_name.empty()) {
        if (auto parsed = Timezone::Parse(scheduled_task.timezone_name); parsed) {
            scheduled_task.timezone_name = parsed->name();
        }
    }
    if (!scheduled_task.cron_expression.empty() && scheduled_task.next_run_epoch_ms <= 0) {
        scheduled_task.next_run_epoch_ms = ComputeNextCronRunEpochMs(
            scheduled_task.cron_expression, scheduled_task.timezone_name, NowEpochMs()).value_or(0);
    }
    scheduled_tasks_.push_back(std::move(scheduled_task));
    flush();
    return scheduled_tasks_.back();
}

bool Scheduler::remove(const std::string& schedule_id) {
    const auto previous_size = scheduled_tasks_.size();
    scheduled_tasks_.erase(std::remove_if(scheduled_tasks_.begin(), scheduled_tasks_.end(), [&](const ScheduledTask& task) {
        return task.schedule_id == schedule_id;
    }), scheduled_tasks_.end());

    const bool removed = previous_size != scheduled_tasks_.size();
    if (removed) {
        flush();
    }
    return removed;
}

std::optional<ScheduledTask> Scheduler::find(const std::string& schedule_id) const {
    const auto it = std::find_if(scheduled_tasks_.begin(), scheduled_tasks_.end(), [&](const ScheduledTask& task) {
        return task.schedule_id == schedule_id;
    });

    if (it == scheduled_tasks_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<ScheduledTask> Scheduler::list() const {
    return scheduled_tasks_;
}

std::vector<ScheduledTask> Scheduler::due(const long long now_epoch_ms) const {
    std::vector<ScheduledTask> due_tasks;
    for (const auto& task : scheduled_tasks_) {
        if (task.enabled && task.next_run_epoch_ms <= now_epoch_ms) {
            due_tasks.push_back(task);
        }
    }
    return due_tasks;
}

std::vector<SchedulerExecutionRecord> Scheduler::run_history() const {
    std::vector<SchedulerExecutionRecord> records;

    std::ifstream input(history_path_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 12) {
            continue;
        }

        records.push_back(SchedulerExecutionRecord{
            .schedule_id = parts[0],
            .task_id = parts[1],
            .started_epoch_ms = ParseLongLong(parts[2], 0),
            .completed_epoch_ms = ParseLongLong(parts[3], 0),
            .run_count = ParseInt(parts[4], 0),
            .success = parts[5] == "1",
            .rescheduled = parts[6] == "1",
            .route_kind = parts[7],
            .route_target = parts[8],
            .error_code = parts[9],
            .error_message = parts[10],
            .duration_ms = ParseInt(parts[11], 0),
        });
    }

    return records;
}

std::vector<SchedulerRunRecord> Scheduler::run_due(AgentLoop& loop, const long long now_epoch_ms) {
    std::vector<SchedulerRunRecord> records;
    bool changed = false;

    for (auto& scheduled_task : scheduled_tasks_) {
        if (!scheduled_task.enabled || scheduled_task.next_run_epoch_ms > now_epoch_ms) {
            continue;
        }

        if (ShouldSkipMissedIntervalRun(scheduled_task, now_epoch_ms)) {
            scheduled_task.next_run_epoch_ms =
                now_epoch_ms + (static_cast<long long>(scheduled_task.interval_seconds) * 1000LL);
            changed = true;
            continue;
        }
        if (ShouldSkipMissedCronRun(scheduled_task, now_epoch_ms)) {
            scheduled_task.next_run_epoch_ms = ComputeNextCronRunEpochMs(
                scheduled_task.cron_expression, scheduled_task.timezone_name,
                now_epoch_ms).value_or(now_epoch_ms + 60000LL);
            changed = true;
            continue;
        }

        auto task = scheduled_task.task;
        task.task_id = scheduled_task.schedule_id + ".run-" + std::to_string(scheduled_task.run_count + 1);
        if (task.idempotency_key.empty()) {
            task.idempotency_key = task.task_id;
        }
        if (task.objective.empty()) {
            task.objective = "Scheduled task " + scheduled_task.schedule_id;
        }

        const auto started_epoch_ms = NowEpochMs();
        const auto result = loop.run(task);
        const auto completed_epoch_ms = NowEpochMs();
        scheduled_task.run_count += 1;

        bool rescheduled = false;
        if (!result.success && scheduled_task.retry_count < scheduled_task.max_retries) {
            scheduled_task.retry_count += 1;
            const auto backoff_ms = static_cast<long long>(scheduled_task.retry_backoff_seconds) * 1000LL;
            scheduled_task.next_run_epoch_ms = now_epoch_ms + (backoff_ms > 0 ? backoff_ms : 1000LL);
            rescheduled = true;
        } else if (!scheduled_task.cron_expression.empty() &&
            (scheduled_task.max_runs == 0 || scheduled_task.run_count < scheduled_task.max_runs)) {
            scheduled_task.retry_count = 0;
            scheduled_task.next_run_epoch_ms = ComputeNextCronRunEpochMs(
                scheduled_task.cron_expression, scheduled_task.timezone_name,
                now_epoch_ms).value_or(now_epoch_ms + 60000LL);
            rescheduled = true;
        } else if (scheduled_task.interval_seconds > 0 &&
            (scheduled_task.max_runs == 0 || scheduled_task.run_count < scheduled_task.max_runs)) {
            scheduled_task.retry_count = 0;
            scheduled_task.next_run_epoch_ms = now_epoch_ms + (static_cast<long long>(scheduled_task.interval_seconds) * 1000LL);
            rescheduled = true;
        } else {
            if (result.success) {
                scheduled_task.retry_count = 0;
            }
            scheduled_task.enabled = false;
        }

        records.push_back(SchedulerRunRecord{
            .schedule_id = scheduled_task.schedule_id,
            .result = result,
            .rescheduled = rescheduled,
        });
        append_execution_record(SchedulerExecutionRecord{
            .schedule_id = scheduled_task.schedule_id,
            .task_id = task.task_id,
            .started_epoch_ms = started_epoch_ms,
            .completed_epoch_ms = completed_epoch_ms,
            .run_count = scheduled_task.run_count,
            .success = result.success,
            .rescheduled = rescheduled,
            .route_kind = route_target_kind_name(result.route_kind),
            .route_target = result.route_target,
            .error_code = result.error_code,
            .error_message = result.error_message,
            .duration_ms = result.duration_ms,
        });
    }

    if (!records.empty() || changed) {
        flush();
    }

    return records;
}

void Scheduler::compact_tasks() const {
    flush();
}

void Scheduler::compact_history() const {
    std::ostringstream output;
    for (const auto& record : run_history()) {
        output
            << EncodeField(record.schedule_id) << kDelimiter
            << EncodeField(record.task_id) << kDelimiter
            << record.started_epoch_ms << kDelimiter
            << record.completed_epoch_ms << kDelimiter
            << record.run_count << kDelimiter
            << (record.success ? "1" : "0") << kDelimiter
            << (record.rescheduled ? "1" : "0") << kDelimiter
            << EncodeField(record.route_kind) << kDelimiter
            << EncodeField(record.route_target) << kDelimiter
            << EncodeField(record.error_code) << kDelimiter
            << EncodeField(record.error_message) << kDelimiter
            << record.duration_ms
            << '\n';
    }
    WriteFileAtomically(history_path_, output.str());
}

const std::filesystem::path& Scheduler::store_path() const {
    return store_path_;
}

const std::filesystem::path& Scheduler::history_path() const {
    return history_path_;
}

long long Scheduler::NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Scheduler::IsCronExpressionValid(const std::string& expression) {
    return CronExpression::Parse(expression).has_value();
}

bool Scheduler::IsTimezoneValid(const std::string& timezone_name) {
    if (timezone_name.empty()) return true;
    return Timezone::Parse(timezone_name).has_value();
}

std::optional<long long> Scheduler::NextCronRunEpochMs(const std::string& expression, const long long after_epoch_ms) {
    return ComputeNextCronRunEpochMs(expression, /*timezone_name=*/"", after_epoch_ms);
}

std::optional<long long> Scheduler::NextCronRunEpochMs(const std::string& expression,
                                                       const std::string& timezone_name,
                                                       const long long after_epoch_ms) {
    return ComputeNextCronRunEpochMs(expression, timezone_name, after_epoch_ms);
}

void Scheduler::load() {
    scheduled_tasks_.clear();

    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 20) {
            continue;
        }

        TaskRequest task{
            .task_id = "",
            .task_type = parts[6],
            .objective = parts[7],
            .workspace_path = parts[8],
            .user_id = parts[9].empty() ? "local-user" : parts[9],
            .idempotency_key = parts[10],
            .remote_trigger = parts[11] == "1",
            .origin_identity_id = parts[12],
            .origin_device_id = parts[13],
            .inputs = ParseInputs(parts[19]),
            .preferred_target = parts[14].empty() ? std::optional<std::string>{} : std::optional<std::string>{parts[14]},
            .timeout_ms = ParseInt(parts[15], 5000),
            .budget_limit = ParseDouble(parts[16], 0.0),
            .allow_high_risk = parts[17] == "1",
            .allow_network = parts[18] == "1",
            .approval_id = parts.size() >= 25 ? parts[24] : "",
            .permission_grants = parts.size() >= 26 ? ParseStringList(parts[25]) : std::vector<std::string>{},
        };

        scheduled_tasks_.push_back(ScheduledTask{
            .schedule_id = parts[0],
            .enabled = parts[1] == "1",
            .next_run_epoch_ms = ParseLongLong(parts[2], 0),
            .interval_seconds = ParseInt(parts[3], 0),
            .max_runs = ParseInt(parts[4], 1),
            .run_count = ParseInt(parts[5], 0),
            .max_retries = parts.size() >= 23 ? ParseInt(parts[20], 0) : 0,
            .retry_count = parts.size() >= 23 ? ParseInt(parts[21], 0) : 0,
            .retry_backoff_seconds = parts.size() >= 23 ? ParseInt(parts[22], 0) : 0,
            .missed_run_policy = parts.size() >= 24 ? NormalizeMissedRunPolicy(parts[23]) : "run-once",
            .cron_expression = parts.size() >= 27 ? parts[26] : "",
            .timezone_name = parts.size() >= 28 ? parts[27] : "",
            .task = std::move(task),
        });
    }
}

void Scheduler::flush() const {
    std::ostringstream output;
    for (const auto& scheduled_task : scheduled_tasks_) {
        const auto& task = scheduled_task.task;
        output
            << EncodeField(scheduled_task.schedule_id) << kDelimiter
            << (scheduled_task.enabled ? "1" : "0") << kDelimiter
            << scheduled_task.next_run_epoch_ms << kDelimiter
            << scheduled_task.interval_seconds << kDelimiter
            << scheduled_task.max_runs << kDelimiter
            << scheduled_task.run_count << kDelimiter
            << EncodeField(task.task_type) << kDelimiter
            << EncodeField(task.objective) << kDelimiter
            << EncodeField(task.workspace_path.string()) << kDelimiter
            << EncodeField(task.user_id) << kDelimiter
            << EncodeField(task.idempotency_key) << kDelimiter
            << (task.remote_trigger ? "1" : "0") << kDelimiter
            << EncodeField(task.origin_identity_id) << kDelimiter
            << EncodeField(task.origin_device_id) << kDelimiter
            << EncodeField(task.preferred_target.value_or("")) << kDelimiter
            << task.timeout_ms << kDelimiter
            << task.budget_limit << kDelimiter
            << (task.allow_high_risk ? "1" : "0") << kDelimiter
            << (task.allow_network ? "1" : "0") << kDelimiter
            << EncodeField(SerializeInputs(task.inputs)) << kDelimiter
            << scheduled_task.max_retries << kDelimiter
            << scheduled_task.retry_count << kDelimiter
            << scheduled_task.retry_backoff_seconds << kDelimiter
            << EncodeField(NormalizeMissedRunPolicy(scheduled_task.missed_run_policy)) << kDelimiter
            << EncodeField(task.approval_id) << kDelimiter
            << EncodeField(SerializeStringList(task.permission_grants)) << kDelimiter
            << EncodeField(scheduled_task.cron_expression) << kDelimiter
            << EncodeField(scheduled_task.timezone_name)
            << '\n';
    }

    WriteFileAtomically(store_path_, output.str());
}

void Scheduler::append_execution_record(const SchedulerExecutionRecord& record) const {
    std::ostringstream output;
    output
        << EncodeField(record.schedule_id) << kDelimiter
        << EncodeField(record.task_id) << kDelimiter
        << record.started_epoch_ms << kDelimiter
        << record.completed_epoch_ms << kDelimiter
        << record.run_count << kDelimiter
        << (record.success ? "1" : "0") << kDelimiter
        << (record.rescheduled ? "1" : "0") << kDelimiter
        << EncodeField(record.route_kind) << kDelimiter
        << EncodeField(record.route_target) << kDelimiter
        << EncodeField(record.error_code) << kDelimiter
        << EncodeField(record.error_message) << kDelimiter
        << record.duration_ms;
    AppendLineToFile(history_path_, output.str());
}

}  // namespace agentos
