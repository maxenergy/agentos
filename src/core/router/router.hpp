#pragma once

#include "core/models.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "memory/memory_manager.hpp"

namespace agentos {

class Router {
public:
    RouteDecision select(
        const TaskRequest& task,
        const SkillRegistry& skill_registry,
        const AgentRegistry& agent_registry,
        const MemoryManager* memory_manager = nullptr) const;
};

}  // namespace agentos
