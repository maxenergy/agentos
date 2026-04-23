#include "core/loop/agent_loop.hpp"

#include <chrono>
#include <sstream>

namespace agentos {

namespace {

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

void ApplyLessonPolicyHint(
    const MemoryManager& memory_manager,
    const TaskRequest& task,
    const std::string& target_name,
    PolicyDecision& decision) {
    if (decision.allowed) {
        return;
    }

    int occurrences = 0;
    std::string summary;
    for (const auto& lesson : memory_manager.lesson_store().list()) {
        if (lesson.enabled &&
            lesson.task_type == task.task_type &&
            lesson.target_name == target_name &&
            lesson.error_code == "PolicyDenied") {
            occurrences += lesson.occurrence_count;
            if (summary.empty()) {
                summary = lesson.summary;
            }
        }
    }

    if (occurrences <= 0) {
        return;
    }

    std::ostringstream hint;
    hint << decision.reason
         << " lesson_hint=previous_policy_denials:"
         << occurrences;
    if (!summary.empty()) {
        hint << " last=\"" << summary << "\"";
    }
    decision.reason = hint.str();
}

}  // namespace

AgentLoop::AgentLoop(
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    Router& router,
    PolicyEngine& policy_engine,
    AuditLogger& audit_logger,
    MemoryManager& memory_manager,
    ExecutionCache& execution_cache)
    : skill_registry_(skill_registry),
      agent_registry_(agent_registry),
      router_(router),
      policy_engine_(policy_engine),
      audit_logger_(audit_logger),
      memory_manager_(memory_manager),
      execution_cache_(execution_cache) {}

TaskRunResult AgentLoop::run(const TaskRequest& task) {
    audit_logger_.record_task_start(task);

    if (!task.idempotency_key.empty()) {
        if (auto cached = execution_cache_.find(task.idempotency_key); cached.has_value()) {
            cached->summary = "Idempotent replay from execution cache. " + cached->summary;
            audit_logger_.record_task_end(task.task_id, *cached);
            memory_manager_.record_task(task, *cached);
            return *cached;
        }
    }

    const auto route = router_.select(task, skill_registry_, agent_registry_, &memory_manager_);
    audit_logger_.record_route(task, route);

    TaskRunResult result;
    if (!route.found()) {
        result.success = false;
        result.summary = "No execution target was selected.";
        result.error_code = "RouteNotFound";
        result.error_message = route.rationale;
        audit_logger_.record_task_end(task.task_id, result);
        memory_manager_.record_task(task, result);
        return result;
    }

    if (route.target_kind == RouteTargetKind::skill) {
        result = run_skill_task(task, route);
    } else {
        result = run_agent_task(task, route);
    }

    audit_logger_.record_task_end(task.task_id, result);
    memory_manager_.record_task(task, result);
    execution_cache_.store(task.idempotency_key, result);
    return result;
}

TaskRunResult AgentLoop::run_skill_task(const TaskRequest& task, const RouteDecision& route) {
    const auto started_at = std::chrono::steady_clock::now();
    auto skill = skill_registry_.find(route.target_name);

    if (!skill) {
        return {
            .success = false,
            .summary = "Selected skill is no longer registered.",
            .route_target = route.target_name,
            .route_kind = RouteTargetKind::skill,
            .error_code = "SkillNotFound",
            .error_message = "Selected skill was not found in SkillRegistry.",
            .duration_ms = ElapsedMs(started_at),
        };
    }

    SkillCall call{
        .call_id = task.task_id + ".skill",
        .skill_name = route.target_name,
        .json_args = "",
        .workspace_id = task.workspace_path.string(),
        .user_id = task.user_id,
        .idempotency_key = task.idempotency_key,
        .arguments = task.inputs,
    };
    if (route.workflow_name.has_value() && !call.arguments.contains("workflow")) {
        call.arguments["workflow"] = *route.workflow_name;
    }

    auto policy = policy_engine_.evaluate_skill(task, skill->manifest(), call);
    ApplyLessonPolicyHint(memory_manager_, task, route.target_name, policy);
    audit_logger_.record_policy(task.task_id, route.target_name, policy);
    if (!policy.allowed) {
        return {
            .success = false,
            .summary = "Policy denied the selected skill.",
            .route_target = route.target_name,
            .route_kind = RouteTargetKind::skill,
            .error_code = "PolicyDenied",
            .error_message = policy.reason,
            .duration_ms = ElapsedMs(started_at),
        };
    }

    const auto skill_result = skill->execute(call);
    TaskStepRecord step{
        .target_kind = RouteTargetKind::skill,
        .target_name = route.target_name,
        .success = skill_result.success,
        .duration_ms = skill_result.duration_ms,
        .summary = skill_result.success ? ("Executed skill " + route.target_name) : "",
        .error_code = skill_result.error_code,
        .error_message = skill_result.error_message,
    };

    audit_logger_.record_step(task.task_id, step);

    return {
        .success = skill_result.success,
        .summary = skill_result.success ? ("Skill " + route.target_name + " completed.") : ("Skill " + route.target_name + " failed."),
        .route_target = route.target_name,
        .route_kind = RouteTargetKind::skill,
        .output_json = skill_result.json_output,
        .error_code = skill_result.error_code,
        .error_message = skill_result.error_message,
        .duration_ms = ElapsedMs(started_at),
        .steps = {step},
    };
}

TaskRunResult AgentLoop::run_agent_task(const TaskRequest& task, const RouteDecision& route) {
    const auto started_at = std::chrono::steady_clock::now();
    auto agent = agent_registry_.find(route.target_name);

    if (!agent) {
        return {
            .success = false,
            .summary = "Selected agent is no longer registered.",
            .route_target = route.target_name,
            .route_kind = RouteTargetKind::agent,
            .error_code = "AgentNotFound",
            .error_message = "Selected agent was not found in AgentRegistry.",
            .duration_ms = ElapsedMs(started_at),
        };
    }

    AgentTask agent_task{
        .task_id = task.task_id,
        .task_type = task.task_type,
        .objective = task.objective,
        .workspace_path = task.workspace_path.string(),
        .context_json = "",
        .constraints_json = "",
        .timeout_ms = task.timeout_ms,
        .budget_limit = task.budget_limit,
    };

    auto policy = policy_engine_.evaluate_agent(task, agent->profile(), agent_task);
    ApplyLessonPolicyHint(memory_manager_, task, route.target_name, policy);
    audit_logger_.record_policy(task.task_id, route.target_name, policy);
    if (!policy.allowed) {
        return {
            .success = false,
            .summary = "Policy denied the selected agent.",
            .route_target = route.target_name,
            .route_kind = RouteTargetKind::agent,
            .error_code = "PolicyDenied",
            .error_message = policy.reason,
            .duration_ms = ElapsedMs(started_at),
        };
    }

    const auto agent_result = agent->run_task(agent_task);
    TaskStepRecord step{
        .target_kind = RouteTargetKind::agent,
        .target_name = route.target_name,
        .success = agent_result.success,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = agent_result.estimated_cost,
        .summary = agent_result.summary,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    };

    audit_logger_.record_step(task.task_id, step);

    return {
        .success = agent_result.success,
        .summary = agent_result.summary,
        .route_target = route.target_name,
        .route_kind = RouteTargetKind::agent,
        .output_json = agent_result.structured_output_json,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
        .duration_ms = ElapsedMs(started_at),
        .steps = {step},
    };
}

}  // namespace agentos
