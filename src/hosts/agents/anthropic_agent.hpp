#pragma once

#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"

#include <atomic>
#include <filesystem>
#include <string>

namespace agentos {

class AnthropicAgent final : public IAgentAdapter {
public:
    AnthropicAgent(
        const CliHost& cli_host,
        const CredentialBroker& credential_broker,
        const AuthProfileStore& profile_store,
        std::filesystem::path workspace_root);

    AgentProfile profile() const override;
    bool healthy() const override;
    std::string start_session(const std::string& session_config_json) override;
    void close_session(const std::string& session_id) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

private:
    std::string profile_name() const;
    static std::string model_name(const AgentTask& task);
    static std::string BuildPrompt(const AgentTask& task);
    static std::string BuildRequestBody(const AgentTask& task);
    static std::string ExtractFirstTextPart(const std::string& response_json);
    AgentResult run_task_with_cli_session(const AgentTask& task, const AuthSession& session, const std::filesystem::path& workspace_path);
    AgentResult run_task_with_rest_session(const AgentTask& task, const AuthSession& session, const std::filesystem::path& workspace_path);

    const CliHost& cli_host_;
    const CredentialBroker& credential_broker_;
    const AuthProfileStore& profile_store_;
    std::filesystem::path workspace_root_;
    std::atomic<int> session_counter_{0};
};

}  // namespace agentos
