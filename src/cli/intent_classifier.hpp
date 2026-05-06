#pragma once

#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace agentos {

// Runtime introspection structs the REPL builds via BuildUsageSnapshot and
// passes into the classifier and skill/agent guides. Defined here so that
// modules outside interactive_commands.cpp (the new skills, the classifier
// itself, and any future REPL helpers) can share the type without pulling
// in interactive_commands.hpp.
struct UsageCommand {
    std::string name;
    std::string syntax;
    std::string description;
    std::vector<std::string> examples;
};

struct RuntimeCapability {
    std::string name;
    std::string description;
    bool available = false;
    std::string status;
};

struct UsageSnapshot {
    std::filesystem::path workspace;
    std::vector<UsageCommand> commands;
    std::vector<RuntimeCapability> agents;
    std::vector<RuntimeCapability> skills;
    std::vector<std::string> tips;
    std::size_t scheduled_tasks = 0;
    std::size_t workflow_candidates = 0;
    std::filesystem::path audit_log;
};

enum class InteractiveRouteKind {
    local_intent,
    direct_skill,
    development_agent,
    research_agent,
    chat_agent,
    unknown_command,
};

std::string RouteKindName(InteractiveRouteKind kind);

enum class InteractiveExecutionMode {
    sync,
    async_job,
};

std::string ExecutionModeName(InteractiveExecutionMode mode);

struct RouteDecisionExplanation {
    std::string task_id;
    std::string user_request;
    InteractiveRouteKind route = InteractiveRouteKind::chat_agent;
    InteractiveExecutionMode execution_mode = InteractiveExecutionMode::sync;
    std::string selected_target;
    int score = 0;
    std::vector<std::string> reasons;
};

// Regex-only intent probes. Used both by ClassifyInteractiveRequest and by
// the REPL's free-form fallbacks (TryHandleNaturalLanguageIntent /
// TryRunRegisteredSkillUseRequest).
bool LooksLikeDevelopmentRequest(const std::string& line);
bool LooksLikeExplicitDevelopmentChangeRequest(const std::string& line);
bool LooksLikeResearchRequest(const std::string& line);
bool LooksLikeRuntimeSelfDescriptionRequest(const std::string& line);
bool LooksLikeHostInfoRequest(const std::string& line);
bool LooksLikeSpecificSkillUsageRequest(const std::string& line,
                                        const SkillRegistry& skill_registry);

// Resolver hooks let the REPL keep its env-var-aware target-selection logic
// in interactive_commands.cpp while still letting the classifier surface the
// chosen target in the route decision JSON. Each callable returns the target
// agent name or empty when none is available.
using TargetResolver = std::function<std::string()>;

// Returns Optional skill manifest name for free-form requests that map to a
// registered skill. Lets the classifier surface direct-skill routes without
// pulling in the heuristics that live in interactive_commands.cpp.
using RegisteredSkillResolver =
    std::function<std::string(const std::string& line)>;
using RegisteredSkillUseChecker =
    std::function<bool(const std::string& line)>;

RouteDecisionExplanation ClassifyInteractiveRequest(
    const std::string& line,
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    const UsageSnapshot& usage_snapshot,
    const std::filesystem::path& workspace,
    const TargetResolver& resolve_chat_target,
    const TargetResolver& resolve_dev_target,
    const TargetResolver& resolve_research_target,
    const RegisteredSkillUseChecker& looks_like_registered_skill_use,
    const RegisteredSkillResolver& resolve_registered_skill);

void WriteRouteDecision(const std::filesystem::path& workspace,
                        const RouteDecisionExplanation& decision);

enum class RuntimeLanguage {
    English,
    Chinese,
};

void PrintRouteDecision(const RouteDecisionExplanation& decision,
                        RuntimeLanguage language);

}  // namespace agentos
