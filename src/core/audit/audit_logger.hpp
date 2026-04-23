#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <mutex>
#include <string>

namespace agentos {

class AuditLogger {
public:
    explicit AuditLogger(std::filesystem::path log_path);

    void record_task_start(const TaskRequest& task);
    void record_route(const TaskRequest& task, const RouteDecision& route);
    void record_policy(const std::string& task_id, const std::string& target_name, const PolicyDecision& decision);
    void record_step(const std::string& task_id, const TaskStepRecord& step);
    void record_task_end(const std::string& task_id, const TaskRunResult& result);
    void record_trust_event(
        const std::string& action,
        const std::string& identity_id,
        const std::string& device_id,
        bool success,
        const std::string& message);

    [[nodiscard]] const std::filesystem::path& log_path() const;

private:
    void append_line(const std::string& line);

    std::filesystem::path log_path_;
    std::mutex mutex_;
};

}  // namespace agentos
