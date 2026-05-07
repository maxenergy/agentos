#pragma once

#include "cli/main_route_action.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/models.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"

#include <filesystem>
#include <functional>

namespace agentos {

TaskRunResult ExecuteMainRouteAction(const MainRouteAction& action,
                                     SkillRegistry& skill_registry,
                                     AgentRegistry& agent_registry,
                                     const std::filesystem::path& workspace,
                                     AgentLoop& loop,
                                     AuditLogger& audit_logger,
                                     const std::function<void(const TaskRunResult&)>& failure_printer = {});

}  // namespace agentos
