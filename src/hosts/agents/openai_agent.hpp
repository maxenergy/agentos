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

// OpenAI Chat Completions adapter implementing both the legacy IAgentAdapter
// and the V2 streaming interface.  Structurally symmetric with QwenAgent but
// targeting the OpenAI API endpoint and its model roster (gpt-4o, o3, etc.).
// Sync path uses curl via CliHost; V2 streaming path spawns curl directly for
// SSE consumption with cancellation support.
class OpenAiAgent final : public IAgentAdapter, public IAgentAdapterV2 {
public:
    OpenAiAgent(
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
    std::optional<std::string> open_session(const StringMap& config) override;

    // Pure projection helper: materializes the legacy AgentTask shape from a
    // V2 AgentInvocation so run_task() can serve as the sync-mode body. Public
    // for direct testing — see tests/agent_provider_tests.cpp's projection
    // regression tests. Every field added to AgentInvocation must also be
    // wired here (see docs/AGENT_SYSTEM.md §4.7 V2 adapter interface).
    static AgentTask InvocationToTask(const AgentInvocation& invocation);

private:
    std::string profile_name(const std::optional<std::string>& requested_profile = std::nullopt) const;
    static std::string model_name(const AgentTask& task);
    static std::string model_name_v2(const AgentInvocation& invocation);
    static std::string BuildPrompt(const AgentTask& task);
    static std::string BuildPromptV2(const AgentInvocation& invocation);
    static std::string BuildRequestBody(const AgentTask& task);
    static std::string BuildRequestBodyV2(const AgentInvocation& invocation, bool stream);
    static std::string ExtractFirstMessageContent(const std::string& response_json);

    const CliHost& cli_host_;
    const CredentialBroker& credential_broker_;
    const AuthProfileStore& profile_store_;
    std::filesystem::path workspace_root_;
    std::atomic<int> session_counter_{0};
};

}  // namespace agentos
