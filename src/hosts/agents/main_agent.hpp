#pragma once

#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"
#include "storage/main_agent_store.hpp"

#include <filesystem>
#include <string>

namespace agentos {

// Primary chat agent. Owns its own auth (env-ref API key or OAuth file)
// and dispatches purely via REST — no CLI passthrough, no shared
// CredentialBroker plumbing. The five sub-agents continue to exist for
// orchestrated specialist work; this adapter is what the REPL chat path
// talks to. Health is "config record loaded + curl on PATH + token
// resolves to a non-empty string" — there is no `auth login main_agent`
// flow, configuration goes through `agentos main-agent set ...`.
class MainAgent : public IAgentAdapter {
public:
    MainAgent(const CliHost& cli_host,
              MainAgentStore store,
              std::filesystem::path workspace_root);

    AgentProfile profile() const override;
    bool healthy() const override;
    std::string start_session(const std::string& session_config_json) override;
    void close_session(const std::string& session_id) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id,
                                    const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

private:
    const CliHost& cli_host_;
    MainAgentStore store_;
    std::filesystem::path workspace_root_;

    // Resolves the bearer/api-key value at request time. Reads
    // env_var when set, falls back to the `access_token` field of
    // oauth_file when only that is configured. Returns empty string
    // and a populated `error` when neither path resolves.
    struct TokenResolution {
        std::string token;
        std::string error_code;
        std::string error_message;
    };
    TokenResolution ResolveToken(const MainAgentConfig& config) const;
};

}  // namespace agentos
