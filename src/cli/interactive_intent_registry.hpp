#pragma once

#include "cli/intent_classifier.hpp"

#include <optional>
#include <string>

namespace agentos {

enum class InteractiveIntentKind {
    main_agent_configure,
    main_agent_inspect,
    runtime_self_description,
    host_info,
    memory_inspect,
    skill_usage,
};

struct InteractiveIntentMatch {
    InteractiveIntentKind intent;
    InteractiveRouteKind route = InteractiveRouteKind::local_intent;
    std::string selected_target = "interactive_runtime";
    std::string reason;
    int score = -5;
};

std::optional<InteractiveIntentMatch> MatchHardLocalInteractiveIntent(
    const std::string& line,
    const SkillRegistry& skill_registry);

bool LooksLikeMainAgentConfigIntent(const std::string& line);
bool LooksLikeModelIdentityIntent(const std::string& line);
bool LooksLikeRuntimeSelfDescriptionIntent(const std::string& line);
bool LooksLikeHostInfoIntent(const std::string& line);
bool LooksLikeMemoryInspectIntent(const std::string& line);
bool LooksLikeSpecificSkillUsageIntent(const std::string& line,
                                       const SkillRegistry& skill_registry);

std::optional<std::string> ExtractOllamaModelName(const std::string& line);

}  // namespace agentos
