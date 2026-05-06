#pragma once

#include "core/models.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
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
//
// Holds non-owning references to the kernel's SkillRegistry and
// AgentRegistry so chat-mode prompts can include a live snapshot of
// what's actually registered (the identity preamble injected in
// BuildPromptText). Both registries are populated incrementally by
// the composition root — agents in particular are registered AFTER
// MainAgent is constructed — so the snapshot must be rebuilt at
// run_task() time, never cached in the constructor.
class MainAgent : public IAgentAdapter {
public:
    MainAgent(const CliHost& cli_host,
              MainAgentStore store,
              std::filesystem::path workspace_root,
              const SkillRegistry& skill_registry,
              const AgentRegistry& agent_registry);

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
    const SkillRegistry* skill_registry_;
    const AgentRegistry* agent_registry_;

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

// Public for testing — builds the prompt text the main agent injects
// into provider requests. For chat task_type, expands the registered
// skills/agents into a grounding preamble (use-when hints, required
// inputs); for other task_types, just emits the orchestration scaffold.
std::string BuildMainAgentPrompt(const AgentTask& task,
                                 const SkillRegistry* skill_registry,
                                 const AgentRegistry* agent_registry);

}  // namespace agentos
