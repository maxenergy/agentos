#include "memory/memory_manager.hpp"

#include "utils/atomic_file.hpp"

#include <fstream>
#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

std::string EncodeField(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r') {
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

RouteTargetKind ParseRouteTargetKind(const std::string& value) {
    if (value == "skill") {
        return RouteTargetKind::skill;
    }
    if (value == "agent") {
        return RouteTargetKind::agent;
    }
    return RouteTargetKind::none;
}

struct WorkflowScoreAccumulator {
    int use_count = 0;
    int success_count = 0;
    int failure_count = 0;
    double total_duration_ms = 0.0;
    // step_signature_counts maps a "step1|step2|..." signature to the number
    // of successful runs that produced exactly that step sequence. Phase 2.2
    // promotion requires a single signature to recur (>=3 occurrences and
    // >=60% of all successful runs of this task_type) instead of trusting
    // whatever the most recent successful run happened to do.
    std::unordered_map<std::string, int> step_signature_counts;
    // Preserves the parsed step list per signature so promotion can emit the
    // argmax signature's ordered_steps without re-splitting strings.
    std::unordered_map<std::string, std::vector<std::string>> steps_by_signature;
};

constexpr int kPromotionMinSignatureCount = 3;
constexpr double kPromotionMinSignatureRatio = 0.6;

std::string MakeStepSignature(const std::vector<std::string>& steps) {
    std::ostringstream output;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (index != 0) {
            output << '|';
        }
        output << steps[index];
    }
    return output.str();
}

double ScoreWorkflow(const WorkflowScoreAccumulator& stats) {
    if (stats.use_count <= 0) {
        return 0.0;
    }

    const auto success_rate = static_cast<double>(stats.success_count) / static_cast<double>(stats.use_count);
    const auto avg_duration_ms = stats.total_duration_ms / static_cast<double>(stats.use_count);
    return (static_cast<double>(stats.success_count) * 100.0 * success_rate) -
           (static_cast<double>(stats.failure_count) * 25.0) -
           (avg_duration_ms / 1000.0);
}

// "sig1:3,sig2:1" — sig values are tab/newline/percent/comma/colon-encoded.
std::string EncodeSignatureCounts(const std::unordered_map<std::string, int>& counts) {
    // Stable order for deterministic TSV output.
    std::vector<std::pair<std::string, int>> entries(counts.begin(), counts.end());
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });

    std::ostringstream output;
    constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        for (const unsigned char ch : entries[index].first) {
            if (ch == '%' || ch == ',' || ch == ':' || ch == '\t' || ch == '\n' || ch == '\r') {
                output << '%' << hex[(ch >> 4) & 0x0F] << hex[ch & 0x0F];
            } else {
                output << static_cast<char>(ch);
            }
        }
        output << ':' << entries[index].second;
    }
    return output.str();
}

}  // namespace

MemoryManager::MemoryManager(std::filesystem::path storage_dir)
    : storage_dir_(std::move(storage_dir)),
      lesson_store_(storage_dir_.empty() ? std::filesystem::path{} : storage_dir_ / "lessons.tsv"),
      workflow_store_(storage_dir_.empty() ? std::filesystem::path{} : storage_dir_ / "workflows.tsv") {
    if (!storage_dir_.empty()) {
        std::filesystem::create_directories(storage_dir_);
        load_persisted_logs();
    }
}

double MemoryManager::UpdateAverage(const double current_average, const int previous_count, const double sample) {
    if (previous_count <= 0) {
        return sample;
    }

    return ((current_average * previous_count) + sample) / static_cast<double>(previous_count + 1);
}

void MemoryManager::record_task(const TaskRequest& task, const TaskRunResult& result) {
    tasks_.push_back(TaskMemoryRecord{
        .task_id = task.task_id,
        .task_type = task.task_type,
        .objective = task.objective,
        .idempotency_key = task.idempotency_key,
        .steps = result.steps,
        .success = result.success,
        .from_cache = result.from_cache,
        .duration_ms = result.duration_ms,
    });

    append_task_log(task, result);

    for (const auto& step : result.steps) {
        append_step_log(task, step);

        if (step.target_kind == RouteTargetKind::skill) {
            auto& stats = skill_stats_[step.target_name];
            const auto previous_count = stats.total_calls;
            stats.total_calls += 1;
            if (step.success) {
                stats.success_calls += 1;
            }
            stats.avg_latency_ms = UpdateAverage(stats.avg_latency_ms, previous_count, step.duration_ms);
        } else if (step.target_kind == RouteTargetKind::agent) {
            auto& stats = agent_stats_[step.target_name];
            const auto previous_count = stats.total_runs;
            stats.total_runs += 1;
            if (step.success) {
                stats.success_runs += 1;
            } else {
                stats.failed_runs += 1;
            }
            stats.avg_duration_ms = UpdateAverage(stats.avg_duration_ms, previous_count, step.duration_ms);
            stats.avg_cost = UpdateAverage(stats.avg_cost, previous_count, step.estimated_cost);
        }
    }

    flush_stats();
    lesson_store_.record_failure(task, result);
    refresh_workflow_store();
}

const std::vector<TaskMemoryRecord>& MemoryManager::task_log() const {
    return tasks_;
}

const std::unordered_map<std::string, SkillStats>& MemoryManager::skill_stats() const {
    return skill_stats_;
}

const std::unordered_map<std::string, AgentRuntimeStats>& MemoryManager::agent_stats() const {
    return agent_stats_;
}

WorkflowStore& MemoryManager::workflow_store() {
    return workflow_store_;
}

const WorkflowStore& MemoryManager::workflow_store() const {
    return workflow_store_;
}

LessonStore& MemoryManager::lesson_store() {
    return lesson_store_;
}

const LessonStore& MemoryManager::lesson_store() const {
    return lesson_store_;
}

const std::filesystem::path& MemoryManager::storage_dir() const {
    return storage_dir_;
}

std::vector<WorkflowCandidate> MemoryManager::workflow_candidates() const {
    std::unordered_map<std::string, WorkflowScoreAccumulator> stats_by_task_type;

    for (const auto& task : tasks_) {
        if (task.from_cache) {
            continue;
        }

        auto& stats = stats_by_task_type[task.task_type];
        stats.use_count += 1;
        stats.total_duration_ms += task.duration_ms;

        if (!task.success || task.steps.empty()) {
            stats.failure_count += 1;
            continue;
        }

        std::vector<std::string> ordered_steps;
        ordered_steps.reserve(task.steps.size());
        for (const auto& step : task.steps) {
            if (step.target_name.empty()) {
                continue;
            }
            ordered_steps.push_back(step.target_name);
        }
        if (ordered_steps.empty()) {
            stats.failure_count += 1;
            continue;
        }

        stats.success_count += 1;
        const auto signature = MakeStepSignature(ordered_steps);
        stats.step_signature_counts[signature] += 1;
        // Only insert the parsed step list the first time we see this signature
        // so promotion picks a stable representative for the argmax winner.
        stats.steps_by_signature.emplace(signature, std::move(ordered_steps));
    }

    std::vector<WorkflowCandidate> workflows;
    for (const auto& [task_type, stats] : stats_by_task_type) {
        if (stats.success_count <= 0 || stats.step_signature_counts.empty()) {
            continue;
        }

        // Pick the signature that has accumulated the most successful runs;
        // ties break alphabetically descending for a deterministic shape.
        const auto winner = std::max_element(
            stats.step_signature_counts.begin(),
            stats.step_signature_counts.end(),
            [](const auto& left, const auto& right) {
                if (left.second != right.second) {
                    return left.second < right.second;
                }
                return left.first > right.first;
            });
        const auto max_signature_count = winner->second;
        const auto max_signature_ratio = static_cast<double>(max_signature_count) /
                                         static_cast<double>(stats.success_count);

        if (max_signature_count < kPromotionMinSignatureCount ||
            max_signature_ratio < kPromotionMinSignatureRatio) {
            continue;
        }

        const auto steps_it = stats.steps_by_signature.find(winner->first);
        if (steps_it == stats.steps_by_signature.end() || steps_it->second.empty()) {
            continue;
        }

        const auto success_rate = static_cast<double>(stats.success_count) / static_cast<double>(stats.use_count);
        const auto avg_duration_ms = stats.total_duration_ms / static_cast<double>(stats.use_count);
        workflows.push_back(WorkflowCandidate{
            .name = task_type + "_workflow",
            .trigger_task_type = task_type,
            .ordered_steps = steps_it->second,
            .use_count = stats.use_count,
            .success_count = stats.success_count,
            .failure_count = stats.failure_count,
            .success_rate = success_rate,
            .avg_duration_ms = avg_duration_ms,
            .score = ScoreWorkflow(stats),
            .step_signature_counts = stats.step_signature_counts,
        });
    }

    std::sort(workflows.begin(), workflows.end(), [](const WorkflowCandidate& left, const WorkflowCandidate& right) {
        if (left.score == right.score) {
            return left.name < right.name;
        }
        return left.score > right.score;
    });

    return workflows;
}

void MemoryManager::refresh_workflow_store() const {
    if (storage_dir_.empty()) {
        return;
    }

    EnsureWorkflowCandidatesSchema();

    std::ostringstream output;
    for (const auto& workflow : workflow_candidates()) {
        std::ostringstream steps;
        for (std::size_t index = 0; index < workflow.ordered_steps.size(); ++index) {
            if (index != 0) {
                steps << ',';
            }
            steps << workflow.ordered_steps[index];
        }

        // Schema v2 appends a 10th column carrying the per-signature recurrence
        // counts that the v2 promotion gate uses ("sig1:3,sig2:1"). Older v1
        // rows with only 9 columns remain parseable: missing column means
        // empty map. tasks_ is rebuilt from task_log.tsv + step_log.tsv on
        // restart, so legacy rows are simply overwritten on the next
        // record_task — no migration of pre-existing rows needed at read time.
        output
            << EncodeField(workflow.name) << kDelimiter
            << EncodeField(workflow.trigger_task_type) << kDelimiter
            << EncodeField(steps.str()) << kDelimiter
            << workflow.use_count << kDelimiter
            << workflow.success_count << kDelimiter
            << workflow.failure_count << kDelimiter
            << workflow.success_rate << kDelimiter
            << workflow.avg_duration_ms << kDelimiter
            << workflow.score << kDelimiter
            << EncodeField(EncodeSignatureCounts(workflow.step_signature_counts))
            << '\n';
    }

    WriteFileAtomically(storage_dir_ / "workflow_candidates.tsv", output.str());
}

void MemoryManager::EnsureWorkflowCandidatesSchema() const {
    // Lightweight per-store schema versioning: bump the file when promotion
    // logic changes shape so external readers can branch on it. We treat any
    // legacy stamp ("1" or absent) as v1 and rewrite to v2; the rewrite is
    // safe because refresh_workflow_store always recomputes candidates from
    // tasks_ (hydrated from task_log + step_log) before writing.
    const auto schema_path = storage_dir_ / ".workflow_candidates_schema_version";
    std::string current;
    if (std::filesystem::exists(schema_path)) {
        std::ifstream input(schema_path, std::ios::binary);
        std::getline(input, current);
    }
    if (current == "2") {
        return;
    }
    WriteFileAtomically(schema_path, std::string("2\n"));
}

void MemoryManager::load_persisted_logs() {
    tasks_.clear();
    skill_stats_.clear();
    agent_stats_.clear();

    std::ifstream tasks_input(storage_dir_ / "task_log.tsv", std::ios::binary);
    std::string line;
    while (std::getline(tasks_input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 11) {
            continue;
        }

        tasks_.push_back(TaskMemoryRecord{
            .task_id = parts[0],
            .task_type = parts[1],
            .objective = parts[2],
            .idempotency_key = parts[3],
            .success = parts[4] == "1",
            .from_cache = parts[5] == "1",
            .duration_ms = std::stoi(parts[6]),
        });
    }

    std::map<std::string, std::vector<TaskStepRecord>> steps_by_task;
    std::ifstream steps_input(storage_dir_ / "step_log.tsv", std::ios::binary);
    while (std::getline(steps_input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 8) {
            continue;
        }

        steps_by_task[parts[0]].push_back(TaskStepRecord{
            .target_kind = ParseRouteTargetKind(parts[1]),
            .target_name = parts[2],
            .success = parts[3] == "1",
            .duration_ms = std::stoi(parts[4]),
            .estimated_cost = parts.size() >= 9 ? std::stod(parts[5]) : 0.0,
            .summary = parts.size() >= 9 ? parts[6] : parts[5],
            .error_code = parts.size() >= 9 ? parts[7] : parts[6],
            .error_message = parts.size() >= 9 ? parts[8] : parts[7],
        });
    }

    for (auto& task : tasks_) {
        if (const auto it = steps_by_task.find(task.task_id); it != steps_by_task.end()) {
            task.steps = it->second;
        }

        for (const auto& step : task.steps) {
            if (step.target_kind == RouteTargetKind::skill) {
                auto& stats = skill_stats_[step.target_name];
                const auto previous_count = stats.total_calls;
                stats.total_calls += 1;
                if (step.success) {
                    stats.success_calls += 1;
                }
                stats.avg_latency_ms = UpdateAverage(stats.avg_latency_ms, previous_count, step.duration_ms);
            } else if (step.target_kind == RouteTargetKind::agent) {
                auto& stats = agent_stats_[step.target_name];
                const auto previous_count = stats.total_runs;
                stats.total_runs += 1;
                if (step.success) {
                    stats.success_runs += 1;
                } else {
                    stats.failed_runs += 1;
                }
                stats.avg_duration_ms = UpdateAverage(stats.avg_duration_ms, previous_count, step.duration_ms);
                stats.avg_cost = UpdateAverage(stats.avg_cost, previous_count, step.estimated_cost);
            }
        }
    }

    flush_stats();
    refresh_workflow_store();
}

void MemoryManager::append_task_log(const TaskRequest& task, const TaskRunResult& result) const {
    if (storage_dir_.empty()) {
        return;
    }

    std::ostringstream output;
    output
        << EncodeField(task.task_id) << kDelimiter
        << EncodeField(task.task_type) << kDelimiter
        << EncodeField(task.objective) << kDelimiter
        << EncodeField(task.idempotency_key) << kDelimiter
        << (result.success ? "1" : "0") << kDelimiter
        << (result.from_cache ? "1" : "0") << kDelimiter
        << result.duration_ms << kDelimiter
        << EncodeField(route_target_kind_name(result.route_kind)) << kDelimiter
        << EncodeField(result.route_target) << kDelimiter
        << EncodeField(result.error_code) << kDelimiter
        << EncodeField(result.error_message);
    AppendLineToFile(storage_dir_ / "task_log.tsv", output.str());
}

void MemoryManager::append_step_log(const TaskRequest& task, const TaskStepRecord& step) const {
    if (storage_dir_.empty()) {
        return;
    }

    std::ostringstream output;
    output
        << EncodeField(task.task_id) << kDelimiter
        << EncodeField(route_target_kind_name(step.target_kind)) << kDelimiter
        << EncodeField(step.target_name) << kDelimiter
        << (step.success ? "1" : "0") << kDelimiter
        << step.duration_ms << kDelimiter
        << step.estimated_cost << kDelimiter
        << EncodeField(step.summary) << kDelimiter
        << EncodeField(step.error_code) << kDelimiter
        << EncodeField(step.error_message);
    AppendLineToFile(storage_dir_ / "step_log.tsv", output.str());
}

void MemoryManager::compact_logs() const {
    if (storage_dir_.empty()) {
        return;
    }

    {
        std::ostringstream output;
        for (const auto& task : tasks_) {
            output
                << EncodeField(task.task_id) << kDelimiter
                << EncodeField(task.task_type) << kDelimiter
                << EncodeField(task.objective) << kDelimiter
                << EncodeField(task.idempotency_key) << kDelimiter
                << (task.success ? "1" : "0") << kDelimiter
                << (task.from_cache ? "1" : "0") << kDelimiter
                << task.duration_ms << kDelimiter
                << EncodeField(task.steps.empty() ? "none" : route_target_kind_name(task.steps.front().target_kind)) << kDelimiter
                << EncodeField(task.steps.empty() ? "" : task.steps.front().target_name) << kDelimiter
                << EncodeField(task.steps.empty() ? "" : task.steps.front().error_code) << kDelimiter
                << EncodeField(task.steps.empty() ? "" : task.steps.front().error_message)
                << '\n';
        }
        WriteFileAtomically(storage_dir_ / "task_log.tsv", output.str());
    }

    {
        std::ostringstream output;
        for (const auto& task : tasks_) {
            for (const auto& step : task.steps) {
                output
                    << EncodeField(task.task_id) << kDelimiter
                    << EncodeField(route_target_kind_name(step.target_kind)) << kDelimiter
                    << EncodeField(step.target_name) << kDelimiter
                    << (step.success ? "1" : "0") << kDelimiter
                    << step.duration_ms << kDelimiter
                    << step.estimated_cost << kDelimiter
                    << EncodeField(step.summary) << kDelimiter
                    << EncodeField(step.error_code) << kDelimiter
                    << EncodeField(step.error_message)
                    << '\n';
            }
        }
        WriteFileAtomically(storage_dir_ / "step_log.tsv", output.str());
    }
}

void MemoryManager::flush_stats() const {
    if (storage_dir_.empty()) {
        return;
    }

    {
        std::ostringstream output;
        for (const auto& [name, stats] : skill_stats_) {
            output
                << EncodeField(name) << kDelimiter
                << stats.total_calls << kDelimiter
                << stats.success_calls << kDelimiter
                << stats.avg_latency_ms << kDelimiter
                << stats.avg_cost << kDelimiter
                << stats.acceptance_rate
                << '\n';
        }
        WriteFileAtomically(storage_dir_ / "skill_stats.tsv", output.str());
    }

    {
        std::ostringstream output;
        for (const auto& [name, stats] : agent_stats_) {
            output
                << EncodeField(name) << kDelimiter
                << stats.total_runs << kDelimiter
                << stats.success_runs << kDelimiter
                << stats.failed_runs << kDelimiter
                << stats.avg_duration_ms << kDelimiter
                << stats.avg_cost << kDelimiter
                << stats.avg_user_score << kDelimiter
                << stats.patch_accept_rate
                << '\n';
        }
        WriteFileAtomically(storage_dir_ / "agent_stats.tsv", output.str());
    }
}

}  // namespace agentos
