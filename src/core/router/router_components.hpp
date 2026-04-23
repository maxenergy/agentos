#pragma once

#include "core/models.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "memory/memory_manager.hpp"

#include <optional>
#include <string>

namespace agentos {

class SkillRouter {
public:
    [[nodiscard]] RouteDecision route_builtin_or_named(
        const TaskRequest& task,
        const SkillRegistry& skill_registry) const;

    [[nodiscard]] bool healthy_skill_exists(
        const SkillRegistry& skill_registry,
        const std::string& name) const;
};

class AgentRouter {
public:
    [[nodiscard]] RouteDecision route_agent_work(
        const TaskRequest& task,
        const AgentRegistry& agent_registry,
        const MemoryManager* memory_manager) const;

    [[nodiscard]] RouteDecision route_named_agent(
        const TaskRequest& task,
        const AgentRegistry& agent_registry) const;

    [[nodiscard]] bool healthy_agent_exists(
        const AgentRegistry& agent_registry,
        const std::string& name) const;
};

class WorkflowRouter {
public:
    [[nodiscard]] RouteDecision route_promoted_workflow(
        const TaskRequest& task,
        const SkillRegistry& skill_registry,
        const MemoryManager* memory_manager) const;
};

}  // namespace agentos
