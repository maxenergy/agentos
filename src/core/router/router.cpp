#include "core/router/router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace agentos {

namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool LooksLikeAgentWork(const std::string& objective) {
    const auto lower = ToLower(objective);
    constexpr std::array<const char*, 4> keywords = {"plan", "analyze", "design", "reason"};

    return std::any_of(keywords.begin(), keywords.end(), [&](const char* keyword) {
        return lower.find(keyword) != std::string::npos;
    });
}

bool HealthySkillExists(const SkillRegistry& registry, const std::string& name) {
    const auto skill = registry.find(name);
    return skill && skill->healthy();
}

bool HealthyAgentExists(const AgentRegistry& registry, const std::string& name) {
    const auto agent = registry.find(name);
    return agent && agent->healthy();
}

std::shared_ptr<IAgentAdapter> BestHealthyAgent(const AgentRegistry& registry, const MemoryManager* memory_manager) {
    if (!memory_manager || memory_manager->agent_stats().empty()) {
        return registry.first_healthy();
    }

    std::shared_ptr<IAgentAdapter> best_agent;
    double best_score = -std::numeric_limits<double>::infinity();

    for (const auto& profile : registry.list_profiles()) {
        const auto agent = registry.find(profile.agent_name);
        if (!agent || !agent->healthy()) {
            continue;
        }

        double score = 0.0;
        if (const auto stats_it = memory_manager->agent_stats().find(profile.agent_name);
            stats_it != memory_manager->agent_stats().end() && stats_it->second.total_runs > 0) {
            const auto& stats = stats_it->second;
            const auto success_rate = static_cast<double>(stats.success_runs) / static_cast<double>(stats.total_runs);
            score = (success_rate * 100.0) - (stats.avg_duration_ms / 1000.0);
        }

        if (!best_agent || score > best_score) {
            best_score = score;
            best_agent = agent;
        }
    }

    return best_agent ? best_agent : registry.first_healthy();
}

std::optional<WorkflowDefinition> BestApplicableWorkflow(
    const TaskRequest& task,
    const SkillRegistry& skill_registry,
    const MemoryManager* memory_manager) {
    if (!memory_manager || !HealthySkillExists(skill_registry, "workflow_run")) {
        return std::nullopt;
    }

    std::optional<WorkflowDefinition> best_workflow;
    for (const auto& workflow : memory_manager->workflow_store().list()) {
        if (!workflow.enabled || workflow.trigger_task_type != task.task_type || workflow.ordered_steps.empty()) {
            continue;
        }
        const auto required_inputs_satisfied = std::all_of(
            workflow.required_inputs.begin(),
            workflow.required_inputs.end(),
            [&](const std::string& input_name) {
                return task.inputs.contains(input_name);
            });
        if (!required_inputs_satisfied) {
            continue;
        }
        if (!best_workflow.has_value() ||
            workflow.score > best_workflow->score ||
            (workflow.score == best_workflow->score && workflow.name < best_workflow->name)) {
            best_workflow = workflow;
        }
    }

    return best_workflow;
}

}  // namespace

RouteDecision Router::select(
    const TaskRequest& task,
    const SkillRegistry& skill_registry,
    const AgentRegistry& agent_registry,
    const MemoryManager* memory_manager) const {
    if (task.preferred_target.has_value()) {
        if (HealthySkillExists(skill_registry, *task.preferred_target)) {
            return {RouteTargetKind::skill, *task.preferred_target, "preferred target matched a registered skill"};
        }
        if (HealthyAgentExists(agent_registry, *task.preferred_target)) {
            return {RouteTargetKind::agent, *task.preferred_target, "preferred target matched a registered agent"};
        }
    }

    if (task.task_type != "workflow_run") {
        if (const auto workflow = BestApplicableWorkflow(task, skill_registry, memory_manager); workflow.has_value()) {
            return {
                RouteTargetKind::skill,
                "workflow_run",
                "promoted workflow matched task_type",
                workflow->name,
            };
        }
    }

    if (task.task_type == "read_file" && HealthySkillExists(skill_registry, "file_read")) {
        return {RouteTargetKind::skill, "file_read", "task_type maps to file_read"};
    }
    if (task.task_type == "write_file" && HealthySkillExists(skill_registry, "file_write")) {
        return {RouteTargetKind::skill, "file_write", "task_type maps to file_write"};
    }
    if (task.task_type == "patch_file" && HealthySkillExists(skill_registry, "file_patch")) {
        return {RouteTargetKind::skill, "file_patch", "task_type maps to file_patch"};
    }

    if (task.task_type == "analysis" || task.task_type == "delegate" || LooksLikeAgentWork(task.objective)) {
        const auto agent = BestHealthyAgent(agent_registry, memory_manager);
        if (agent) {
            return {
                RouteTargetKind::agent,
                agent->profile().agent_name,
                memory_manager && !memory_manager->agent_stats().empty()
                    ? "objective routed by agent health and historical score"
                    : "objective is better handled by an agent adapter",
            };
        }
    }

    if (HealthySkillExists(skill_registry, task.task_type)) {
        return {RouteTargetKind::skill, task.task_type, "task_type matched a skill name directly"};
    }

    if (HealthyAgentExists(agent_registry, task.task_type)) {
        return {RouteTargetKind::agent, task.task_type, "task_type matched an agent name directly"};
    }

    return {RouteTargetKind::none, "", "no route matched the current registries"};
}

}  // namespace agentos
