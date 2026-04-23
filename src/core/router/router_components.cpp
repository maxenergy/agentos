#include "core/router/router_components.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>

namespace agentos {

namespace {

constexpr int kRepeatedLessonThreshold = 2;

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

int LessonOccurrenceCount(
    const MemoryManager* memory_manager,
    const std::string& task_type,
    const std::string& target_name) {
    if (!memory_manager) {
        return 0;
    }

    int occurrences = 0;
    for (const auto& lesson : memory_manager->lesson_store().list()) {
        if (lesson.enabled && lesson.task_type == task_type && lesson.target_name == target_name) {
            occurrences += lesson.occurrence_count;
        }
    }
    return occurrences;
}

bool HasRepeatedLesson(
    const MemoryManager* memory_manager,
    const std::string& task_type,
    const std::string& target_name) {
    return LessonOccurrenceCount(memory_manager, task_type, target_name) >= kRepeatedLessonThreshold;
}

bool HasRuntimeHistory(const MemoryManager* memory_manager) {
    return memory_manager &&
           (!memory_manager->agent_stats().empty() ||
            !memory_manager->lesson_store().list().empty());
}

std::shared_ptr<IAgentAdapter> BestHealthyAgent(
    const AgentRegistry& registry,
    const MemoryManager* memory_manager,
    const std::string& task_type) {
    if (!HasRuntimeHistory(memory_manager)) {
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
        score -= static_cast<double>(LessonOccurrenceCount(memory_manager, task_type, profile.agent_name)) * 25.0;

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
    const MemoryManager* memory_manager,
    const SkillRouter& skill_router) {
    if (!memory_manager || !skill_router.healthy_skill_exists(skill_registry, "workflow_run")) {
        return std::nullopt;
    }
    if (HasRepeatedLesson(memory_manager, task.task_type, "workflow_run")) {
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

RouteDecision SkillRouter::route_builtin_or_named(
    const TaskRequest& task,
    const SkillRegistry& skill_registry) const {
    if (task.task_type == "read_file" && healthy_skill_exists(skill_registry, "file_read")) {
        return {RouteTargetKind::skill, "file_read", "task_type maps to file_read"};
    }
    if (task.task_type == "write_file" && healthy_skill_exists(skill_registry, "file_write")) {
        return {RouteTargetKind::skill, "file_write", "task_type maps to file_write"};
    }
    if (task.task_type == "patch_file" && healthy_skill_exists(skill_registry, "file_patch")) {
        return {RouteTargetKind::skill, "file_patch", "task_type maps to file_patch"};
    }

    if (healthy_skill_exists(skill_registry, task.task_type)) {
        return {RouteTargetKind::skill, task.task_type, "task_type matched a skill name directly"};
    }

    return {RouteTargetKind::none, "", "no skill route matched"};
}

bool SkillRouter::healthy_skill_exists(
    const SkillRegistry& skill_registry,
    const std::string& name) const {
    const auto skill = skill_registry.find(name);
    return skill && skill->healthy();
}

RouteDecision AgentRouter::route_agent_work(
    const TaskRequest& task,
    const AgentRegistry& agent_registry,
    const MemoryManager* memory_manager) const {
    if (task.task_type != "analysis" && task.task_type != "delegate" && !LooksLikeAgentWork(task.objective)) {
        return {RouteTargetKind::none, "", "task does not require agent routing"};
    }

    const auto agent = BestHealthyAgent(agent_registry, memory_manager, task.task_type);
    if (!agent) {
        return {RouteTargetKind::none, "", "no healthy agent route matched"};
    }

    return {
        RouteTargetKind::agent,
        agent->profile().agent_name,
        HasRuntimeHistory(memory_manager)
            ? "objective routed by agent health, historical score, and lessons"
            : "objective is better handled by an agent adapter",
    };
}

RouteDecision AgentRouter::route_named_agent(
    const TaskRequest& task,
    const AgentRegistry& agent_registry) const {
    if (healthy_agent_exists(agent_registry, task.task_type)) {
        return {RouteTargetKind::agent, task.task_type, "task_type matched an agent name directly"};
    }

    return {RouteTargetKind::none, "", "no named agent route matched"};
}

bool AgentRouter::healthy_agent_exists(
    const AgentRegistry& agent_registry,
    const std::string& name) const {
    const auto agent = agent_registry.find(name);
    return agent && agent->healthy();
}

RouteDecision WorkflowRouter::route_promoted_workflow(
    const TaskRequest& task,
    const SkillRegistry& skill_registry,
    const MemoryManager* memory_manager) const {
    if (task.task_type == "workflow_run") {
        return {RouteTargetKind::none, "", "workflow_run tasks should route directly"};
    }

    const SkillRouter skill_router;
    if (const auto workflow = BestApplicableWorkflow(task, skill_registry, memory_manager, skill_router);
        workflow.has_value()) {
        return {
            RouteTargetKind::skill,
            "workflow_run",
            "promoted workflow matched task_type",
            workflow->name,
        };
    }

    return {RouteTargetKind::none, "", "no promoted workflow route matched"};
}

}  // namespace agentos
