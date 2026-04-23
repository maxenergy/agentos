#pragma once

#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/models.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "memory/memory_manager.hpp"

namespace agentos {

class AgentLoop {
public:
    AgentLoop(
        SkillRegistry& skill_registry,
        AgentRegistry& agent_registry,
        Router& router,
        PolicyEngine& policy_engine,
        AuditLogger& audit_logger,
        MemoryManager& memory_manager,
        ExecutionCache& execution_cache);

    TaskRunResult run(const TaskRequest& task);

private:
    TaskRunResult run_skill_task(const TaskRequest& task, const RouteDecision& route);
    TaskRunResult run_agent_task(const TaskRequest& task, const RouteDecision& route);

    SkillRegistry& skill_registry_;
    AgentRegistry& agent_registry_;
    Router& router_;
    PolicyEngine& policy_engine_;
    AuditLogger& audit_logger_;
    MemoryManager& memory_manager_;
    ExecutionCache& execution_cache_;
};

}  // namespace agentos
