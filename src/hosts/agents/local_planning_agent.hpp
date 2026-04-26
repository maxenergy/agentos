#pragma once

#include "core/models.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>

namespace agentos {

class LocalPlanningAgent final : public IAgentAdapter {
public:
    AgentProfile profile() const override;
    bool healthy() const override;
    std::string start_session(const std::string& session_config_json) override;
    void close_session(const std::string& session_id) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

private:
    [[nodiscard]] bool is_cancelled(const std::string& task_id) const;

    std::atomic<int> session_counter_{0};
    mutable std::mutex mutex_;
    std::unordered_set<std::string> active_sessions_;
    std::unordered_set<std::string> cancelled_tasks_;
};

}  // namespace agentos
