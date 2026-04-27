#pragma once

#include "core/models.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

namespace agentos {

// Phase 4.1 reference V2 adapter. Inherits both interfaces so legacy callers
// (SubagentManager, AgentRegistry) keep working through the IAgentAdapter
// shim while V2 callers can drive invoke()/CancellationToken directly.
// profile()/healthy()/close_session() share signatures across both interfaces,
// so a single override satisfies both bases.
class LocalPlanningAgent final : public IAgentAdapter, public IAgentAdapterV2 {
public:
    AgentProfile profile() const override;
    bool healthy() const override;

    // Legacy IAgentAdapter surface — translates to/from invoke().
    std::string start_session(const std::string& session_config_json) override;
    void close_session(const std::string& session_id) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

    // V2 surface.
    AgentResult invoke(const AgentInvocation& invocation,
                       const AgentEventCallback& on_event = {}) override;
    std::optional<std::string> open_session(const StringMap& config) override;

private:
    std::string allocate_session_id();

    std::atomic<int> session_counter_{0};
    mutable std::mutex mutex_;
    std::unordered_set<std::string> active_sessions_;
};

}  // namespace agentos
