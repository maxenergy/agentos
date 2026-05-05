#include "core/orchestration/agent_dispatch.hpp"

#include "core/audit/audit_logger.hpp"
#include "core/policy/policy_engine.hpp"
#include "memory/lesson_hints.hpp"
#include "memory/memory_manager.hpp"
#include "utils/cancellation.hpp"

#include <chrono>
#include <memory>
#include <utility>

namespace agentos {
namespace {

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

TaskStepRecord BuildFailureStep(
    const std::string& agent_name,
    const int duration_ms,
    std::string error_code,
    std::string error_message) {
    return {
        .target_kind = RouteTargetKind::agent,
        .target_name = agent_name,
        .success = false,
        .duration_ms = duration_ms,
        .error_code = std::move(error_code),
        .error_message = std::move(error_message),
    };
}

AgentDispatchResult FromFailureStep(TaskStepRecord step) {
    AgentDispatchResult result;
    result.success = false;
    result.duration_ms = step.duration_ms;
    result.error_code = step.error_code;
    result.error_message = step.error_message;
    result.step = std::move(step);
    return result;
}

}  // namespace

AgentDispatchResult DispatchAgent(
    const AgentDispatchInput& input,
    PolicyEngine& policy_engine,
    AuditLogger& audit_logger,
    MemoryManager& memory_manager) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto agent_name = input.agent_name.empty() && input.agent
        ? input.agent->profile().agent_name
        : input.agent_name;

    if (!input.agent || !input.agent->healthy()) {
        return FromFailureStep(BuildFailureStep(
            agent_name,
            ElapsedMs(started_at),
            "AgentUnavailable",
            "Agent was not found or is unhealthy."));
    }

    const auto profile = input.agent->profile();
    const auto effective_task_id = input.agent_task_id.empty() ? input.task.task_id : input.agent_task_id;
    const auto effective_objective = input.objective.empty() ? input.task.objective : input.objective;
    AgentTask agent_task{
        .task_id = effective_task_id,
        .task_type = input.task.task_type,
        .objective = effective_objective,
        .workspace_path = input.task.workspace_path.string(),
        .auth_profile = input.task.auth_profile,
        .context_json = input.context_json,
        .constraints_json = input.constraints_json,
        .timeout_ms = input.task.timeout_ms,
        .budget_limit = input.task.budget_limit,
    };

    auto policy = policy_engine.evaluate_agent(input.task, profile, agent_task);
    ApplyLessonPolicyHint(memory_manager, input.task, profile.agent_name, policy);
    audit_logger.record_policy(input.task.task_id, profile.agent_name, policy);
    if (!policy.allowed) {
        return FromFailureStep(BuildFailureStep(
            profile.agent_name,
            ElapsedMs(started_at),
            "PolicyDenied",
            policy.reason));
    }

    if (input.cancel && input.cancel->is_cancelled()) {
        return FromFailureStep(BuildFailureStep(
            profile.agent_name,
            ElapsedMs(started_at),
            "Cancelled",
            "agent dispatch was cancelled by the orchestrator"));
    }

    AgentResult agent_result;
    if (auto* v2 = dynamic_cast<IAgentAdapterV2*>(input.agent.get())) {
        AgentInvocation invocation;
        invocation.task_id = agent_task.task_id;
        invocation.objective = agent_task.objective;
        invocation.workspace_path = input.task.workspace_path;
        invocation.auth_profile = input.task.auth_profile;
        invocation.context = input.invocation_context;
        invocation.constraints = input.invocation_constraints;
        invocation.timeout_ms = input.task.timeout_ms;
        invocation.budget_limit_usd = input.task.budget_limit;
        invocation.resume_session_id = input.resume_session_id;
        invocation.cancel = input.cancel;
        agent_result = v2->invoke(invocation, input.on_agent_event);
    } else {
        agent_result = input.agent->run_task(agent_task);
    }

    const double effective_cost = agent_result.usage.cost_usd > 0.0
        ? agent_result.usage.cost_usd
        : agent_result.estimated_cost;
    TaskStepRecord step{
        .target_kind = RouteTargetKind::agent,
        .target_name = profile.agent_name,
        .success = agent_result.success,
        .duration_ms = agent_result.duration_ms > 0 ? agent_result.duration_ms : ElapsedMs(started_at),
        .estimated_cost = effective_cost,
        .summary = agent_result.summary,
        .structured_output_json = agent_result.structured_output_json,
        .artifacts = agent_result.artifacts,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    };

    return {
        .success = agent_result.success,
        .step = step,
        .structured_output_json = agent_result.structured_output_json,
        .artifacts = agent_result.artifacts,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
        .duration_ms = step.duration_ms,
        .effective_cost = effective_cost,
    };
}

}  // namespace agentos
