#pragma once

#include "core/models.hpp"

#include <optional>
#include <string>

namespace agentos {

class AgentRegistry;
class SkillRegistry;

struct MainRouteAction {
    std::string action;
    std::string target_kind;
    std::string target;
    std::string brief;
    std::string mode = "sync";
    StringMap arguments;
};

struct MainRouteActionValidation {
    bool valid = true;
    std::string error_code;
    std::string error_message;
};

std::optional<MainRouteAction> ParseMainRouteAction(const std::string& text);

MainRouteActionValidation ValidateMainRouteAction(const MainRouteAction& action,
                                                  const SkillRegistry& skill_registry,
                                                  const AgentRegistry& agent_registry);

std::string BuildRouteActionResultPrompt(const std::string& original_prompt,
                                         const MainRouteAction& action,
                                         const TaskRunResult& result);

}  // namespace agentos
