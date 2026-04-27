#pragma once

#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"

#include <atomic>
#include <filesystem>

namespace agentos {

// Phase 4.2: implements both the legacy IAgentAdapter (existing call sites) and
// the V2 streaming interface (NDJSON event stream + cancellation token).
// profile()/healthy()/close_session(string) have matching signatures across
// both interfaces, so single overrides satisfy both vtables.
class CodexCliAgent final : public IAgentAdapter, public IAgentAdapterV2 {
public:
    CodexCliAgent(const CliHost& cli_host, std::filesystem::path workspace_root);

    // Shared by IAgentAdapter and IAgentAdapterV2.
    AgentProfile profile() const override;
    bool healthy() const override;
    void close_session(const std::string& session_id) override;

    // IAgentAdapter only.
    std::string start_session(const std::string& session_config_json) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

    // IAgentAdapterV2 only.
    AgentResult invoke(const AgentInvocation& invocation,
                       const AgentEventCallback& on_event = {}) override;

private:
    static std::string BuildPrompt(const AgentTask& task);
    static std::string BuildPromptV2(const AgentInvocation& invocation);
    static std::string SafeFileStem(std::string value);

    const CliHost& cli_host_;
    std::filesystem::path workspace_root_;
    std::atomic<int> session_counter_{0};
};

}  // namespace agentos
