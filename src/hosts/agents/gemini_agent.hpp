#pragma once

#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

namespace agentos {

// Implements both the legacy IAgentAdapter and the V2 streaming interface.
// Gemini's REST + CLI paths are non-streaming today, so invoke() wraps the
// existing run_task path with cancellation checks and SessionInit/Final
// events — closer to LocalPlanningAgent's pattern than Qwen's full SSE.
// profile()/healthy()/close_session(string) match across both interfaces, so
// single overrides satisfy both vtables.
class GeminiAgent final : public IAgentAdapter, public IAgentAdapterV2 {
public:
    GeminiAgent(
        const CliHost& cli_host,
        const CredentialBroker& credential_broker,
        const AuthProfileStore& profile_store,
        std::filesystem::path workspace_root);

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
    std::string profile_name() const;
    static std::string model_name(const AgentTask& task);
    static std::string ModelNameFromConstraints(const StringMap& constraints);
    static std::string BuildPrompt(const AgentTask& task);
    static std::string BuildRequestBody(const AgentTask& task);
    static std::string ExtractFirstTextPart(const std::string& response_json);
    static AgentTask TaskFromInvocation(const AgentInvocation& invocation);

    AgentResult run_task_with_cli_session(const AgentTask& task, const AuthSession& session, const std::filesystem::path& workspace_path);
    AgentResult run_task_with_rest_session(const AgentTask& task, const AuthSession& session, const std::filesystem::path& workspace_path);

    const CliHost& cli_host_;
    const CredentialBroker& credential_broker_;
    const AuthProfileStore& profile_store_;
    std::filesystem::path workspace_root_;
    std::atomic<int> session_counter_{0};
};

}  // namespace agentos
