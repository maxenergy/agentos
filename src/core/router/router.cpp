#include "core/router/router.hpp"

#include "core/router/router_components.hpp"

namespace agentos {

RouteDecision Router::select(
    const TaskRequest& task,
    const SkillRegistry& skill_registry,
    const AgentRegistry& agent_registry,
    const MemoryManager* memory_manager) const {
    const SkillRouter skill_router;
    const AgentRouter agent_router;
    const WorkflowRouter workflow_router;

    if (task.preferred_target.has_value()) {
        if (skill_router.healthy_skill_exists(skill_registry, *task.preferred_target)) {
            return {RouteTargetKind::skill, *task.preferred_target, "preferred target matched a registered skill"};
        }
        if (agent_router.healthy_agent_exists(agent_registry, *task.preferred_target)) {
            return {RouteTargetKind::agent, *task.preferred_target, "preferred target matched a registered agent"};
        }
    }

    if (const auto workflow_route = workflow_router.route_promoted_workflow(task, skill_registry, memory_manager);
        workflow_route.found()) {
        return workflow_route;
    }

    if (const auto skill_route = skill_router.route_builtin_or_named(task, skill_registry);
        skill_route.found()) {
        return skill_route;
    }

    if (const auto agent_work_route = agent_router.route_agent_work(task, agent_registry, memory_manager);
        agent_work_route.found()) {
        return agent_work_route;
    }

    if (const auto named_agent_route = agent_router.route_named_agent(task, agent_registry);
        named_agent_route.found()) {
        return named_agent_route;
    }

    return {RouteTargetKind::none, "", "no route matched the current registries"};
}

}  // namespace agentos
