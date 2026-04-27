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

// Phase 4.4: implements both the legacy IAgentAdapter (existing call sites) and
// the V2 streaming interface (Anthropic Messages SSE event stream + cancellation
// token). profile()/healthy()/close_session(string) have matching signatures
// across both interfaces, so single overrides satisfy both vtables.
class AnthropicAgent final : public IAgentAdapter, public IAgentAdapterV2 {
public:
    AnthropicAgent(
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

    // Pure projection helper: materializes the legacy AgentTask shape from a
    // V2 AgentInvocation so run_task* paths can serve as the sync-mode body.
    // Public for direct testing — see tests/agent_provider_tests.cpp's
    // projection regression tests. Every field added to AgentInvocation must
    // also be wired here (see docs/AGENT_SYSTEM.md §4.7 V2 adapter interface).
    static AgentTask TaskFromInvocation(const AgentInvocation& invocation);

private:
    std::string profile_name(const std::optional<std::string>& requested_profile = std::nullopt) const;
    static std::string model_name(const AgentTask& task);
    static std::string ModelNameFromConstraints(const StringMap& constraints);
    static std::string BuildPrompt(const AgentTask& task);
    static std::string BuildPromptV2(const AgentInvocation& invocation);
    static std::string BuildRequestBody(const AgentTask& task);
    static std::string BuildRequestBodyV2(const AgentInvocation& invocation, bool stream);
    static std::string ExtractFirstTextPart(const std::string& response_json);

    AgentResult run_task_with_cli_session(const AgentTask& task, const AuthSession& session, const std::filesystem::path& workspace_path);
    AgentResult run_task_with_rest_session(const AgentTask& task, const AuthSession& session, const std::filesystem::path& workspace_path);

    // Streaming SSE invocation against /v1/messages with stream=true. Returns
    // nullopt if the stream attempt failed early (e.g. curl missing, secret
    // staging failed, or curl exited non-zero before producing a final event)
    // so the caller can fall back to the non-streaming path.
    std::optional<AgentResult> invoke_with_rest_streaming(
        const AgentInvocation& invocation,
        const AuthSession& session,
        const std::filesystem::path& workspace_path,
        const AgentEventCallback& on_event);

    const CliHost& cli_host_;
    const CredentialBroker& credential_broker_;
    const AuthProfileStore& profile_store_;
    std::filesystem::path workspace_root_;
    std::atomic<int> session_counter_{0};
};

}  // namespace agentos
