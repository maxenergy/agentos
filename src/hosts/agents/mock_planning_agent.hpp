#pragma once

#include "core/models.hpp"

#include <atomic>

namespace agentos {

class MockPlanningAgent final : public IAgentAdapter {
public:
    AgentProfile profile() const override;
    bool healthy() const override;
    std::string start_session(const std::string& session_config_json) override;
    void close_session(const std::string& session_id) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

private:
    std::atomic<int> session_counter_{0};
};

}  // namespace agentos

