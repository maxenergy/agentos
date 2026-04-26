#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

class AuditLogger {
public:
    explicit AuditLogger(std::filesystem::path log_path);

    void record_task_start(const TaskRequest& task);
    void record_route(const TaskRequest& task, const RouteDecision& route);
    void record_policy(const std::string& task_id, const std::string& target_name, const PolicyDecision& decision);
    void record_step(const std::string& task_id, const TaskStepRecord& step);
    void record_task_end(const std::string& task_id, const TaskRunResult& result);
    void record_config_diagnostic(
        const std::string& source,
        const std::filesystem::path& file,
        int line_number,
        const std::string& reason);
    void record_trust_event(
        const std::string& action,
        const std::string& identity_id,
        const std::string& device_id,
        bool success,
        const std::string& message);
    void compact_log(const std::filesystem::path& recovery_storage_dir = {});

    [[nodiscard]] const std::filesystem::path& log_path() const;

private:
    std::vector<std::string> sensitive_values_for_task(const std::string& task_id);
    std::string redact_for_task(const std::string& task_id, std::string value);
    void append_line(const std::string& line);

    std::filesystem::path log_path_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::string>> sensitive_values_by_task_;
};

}  // namespace agentos
