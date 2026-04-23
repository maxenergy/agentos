#include "core/audit/audit_logger.hpp"

#include "utils/json_utils.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace agentos {

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

}  // namespace

AuditLogger::AuditLogger(std::filesystem::path log_path)
    : log_path_(std::move(log_path)) {
    std::filesystem::create_directories(log_path_.parent_path());
}

void AuditLogger::record_task_start(const TaskRequest& task) {
    append_line(MakeJsonObject({
        {"ts", QuoteJson(CurrentTimestamp())},
        {"event", QuoteJson("task_start")},
        {"task_id", QuoteJson(task.task_id)},
        {"task_type", QuoteJson(task.task_type)},
        {"objective", QuoteJson(task.objective)},
        {"workspace", QuoteJson(task.workspace_path.string())},
    }));
}

void AuditLogger::record_route(const TaskRequest& task, const RouteDecision& route) {
    append_line(MakeJsonObject({
        {"ts", QuoteJson(CurrentTimestamp())},
        {"event", QuoteJson("route")},
        {"task_id", QuoteJson(task.task_id)},
        {"target_kind", QuoteJson(route_target_kind_name(route.target_kind))},
        {"target_name", QuoteJson(route.target_name)},
        {"rationale", QuoteJson(route.rationale)},
    }));
}

void AuditLogger::record_policy(
    const std::string& task_id,
    const std::string& target_name,
    const PolicyDecision& decision) {
    append_line(MakeJsonObject({
        {"ts", QuoteJson(CurrentTimestamp())},
        {"event", QuoteJson("policy")},
        {"task_id", QuoteJson(task_id)},
        {"target_name", QuoteJson(target_name)},
        {"allowed", BoolAsJson(decision.allowed)},
        {"reason", QuoteJson(decision.reason)},
    }));
}

void AuditLogger::record_step(const std::string& task_id, const TaskStepRecord& step) {
    append_line(MakeJsonObject({
        {"ts", QuoteJson(CurrentTimestamp())},
        {"event", QuoteJson("step")},
        {"task_id", QuoteJson(task_id)},
        {"target_kind", QuoteJson(route_target_kind_name(step.target_kind))},
        {"target_name", QuoteJson(step.target_name)},
        {"success", BoolAsJson(step.success)},
        {"duration_ms", NumberAsJson(step.duration_ms)},
        {"summary", QuoteJson(step.summary)},
        {"error_code", QuoteJson(step.error_code)},
        {"error_message", QuoteJson(step.error_message)},
    }));
}

void AuditLogger::record_task_end(const std::string& task_id, const TaskRunResult& result) {
    append_line(MakeJsonObject({
        {"ts", QuoteJson(CurrentTimestamp())},
        {"event", QuoteJson("task_end")},
        {"task_id", QuoteJson(task_id)},
        {"success", BoolAsJson(result.success)},
        {"from_cache", BoolAsJson(result.from_cache)},
        {"route_kind", QuoteJson(route_target_kind_name(result.route_kind))},
        {"route_target", QuoteJson(result.route_target)},
        {"duration_ms", NumberAsJson(result.duration_ms)},
        {"summary", QuoteJson(result.summary)},
        {"error_code", QuoteJson(result.error_code)},
        {"error_message", QuoteJson(result.error_message)},
    }));
}

void AuditLogger::record_trust_event(
    const std::string& action,
    const std::string& identity_id,
    const std::string& device_id,
    const bool success,
    const std::string& message) {
    append_line(MakeJsonObject({
        {"ts", QuoteJson(CurrentTimestamp())},
        {"event", QuoteJson("trust")},
        {"action", QuoteJson(action)},
        {"identity_id", QuoteJson(identity_id)},
        {"device_id", QuoteJson(device_id)},
        {"success", BoolAsJson(success)},
        {"message", QuoteJson(message)},
    }));
}

const std::filesystem::path& AuditLogger::log_path() const {
    return log_path_;
}

void AuditLogger::append_line(const std::string& line) {
    std::lock_guard<std::mutex> guard(mutex_);

    std::ofstream output(log_path_, std::ios::app | std::ios::binary);
    output << line << '\n';
}

}  // namespace agentos
