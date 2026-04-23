#include "core/orchestration/subagent_manager.hpp"

#include "utils/json_utils.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <sstream>
#include <unordered_set>

namespace agentos {

namespace {

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

std::vector<std::string> NormalizeAgentNames(const std::vector<std::string>& agent_names) {
    std::vector<std::string> normalized;
    std::unordered_set<std::string> seen;

    for (const auto& agent_name : agent_names) {
        if (agent_name.empty() || seen.contains(agent_name)) {
            continue;
        }

        seen.insert(agent_name);
        normalized.push_back(agent_name);
    }

    return normalized;
}

std::string JoinAgentNames(const std::vector<std::string>& agent_names) {
    std::ostringstream output;
    for (std::size_t index = 0; index < agent_names.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << agent_names[index];
    }
    return output.str();
}

std::string BuildSummary(const std::size_t success_count, const std::size_t total_count) {
    return "Subagent orchestration completed: " + std::to_string(success_count) + "/" +
           std::to_string(total_count) + " succeeded.";
}

std::string BuildOutputJson(
    const std::vector<std::string>& agent_names,
    const std::size_t success_count,
    const std::size_t total_count) {
    return MakeJsonObject({
        {"agents", QuoteJson(JoinAgentNames(agent_names))},
        {"success_count", NumberAsJson(static_cast<int>(success_count))},
        {"total_count", NumberAsJson(static_cast<int>(total_count))},
    });
}

}  // namespace

std::string ToString(const SubagentExecutionMode mode) {
    switch (mode) {
    case SubagentExecutionMode::parallel:
        return "parallel";
    case SubagentExecutionMode::sequential:
    default:
        return "sequential";
    }
}

SubagentExecutionMode ParseSubagentExecutionMode(const std::string& value) {
    if (value == "parallel") {
        return SubagentExecutionMode::parallel;
    }
    return SubagentExecutionMode::sequential;
}

SubagentManager::SubagentManager(
    AgentRegistry& agent_registry,
    PolicyEngine& policy_engine,
    AuditLogger& audit_logger,
    MemoryManager& memory_manager,
    const std::size_t max_subagents)
    : agent_registry_(agent_registry),
      policy_engine_(policy_engine),
      audit_logger_(audit_logger),
      memory_manager_(memory_manager),
      max_subagents_(max_subagents) {}

TaskRunResult SubagentManager::run(
    const TaskRequest& task,
    const std::vector<std::string>& agent_names,
    const SubagentExecutionMode mode) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto normalized_agent_names = NormalizeAgentNames(agent_names);

    audit_logger_.record_task_start(task);
    audit_logger_.record_route(task, RouteDecision{
        .target_kind = RouteTargetKind::agent,
        .target_name = "subagents",
        .rationale = "explicit subagent orchestration in " + ToString(mode) + " mode",
    });

    TaskRunResult result{
        .route_target = "subagents",
        .route_kind = RouteTargetKind::agent,
    };

    if (normalized_agent_names.empty()) {
        result.success = false;
        result.summary = "No subagents were selected.";
        result.error_code = "SubagentRouteNotFound";
        result.error_message = "agents=<name[,name]> is required for subagent orchestration.";
        result.duration_ms = ElapsedMs(started_at);
        audit_logger_.record_task_end(task.task_id, result);
        memory_manager_.record_task(task, result);
        return result;
    }

    if (normalized_agent_names.size() > max_subagents_) {
        result.success = false;
        result.summary = "Too many subagents were requested.";
        result.error_code = "TooManySubagents";
        result.error_message = "Requested " + std::to_string(normalized_agent_names.size()) +
                               " subagents; max is " + std::to_string(max_subagents_) + ".";
        result.duration_ms = ElapsedMs(started_at);
        audit_logger_.record_task_end(task.task_id, result);
        memory_manager_.record_task(task, result);
        return result;
    }

    if (mode == SubagentExecutionMode::parallel) {
        std::vector<std::future<TaskStepRecord>> futures;
        futures.reserve(normalized_agent_names.size());
        for (const auto& agent_name : normalized_agent_names) {
            futures.push_back(std::async(std::launch::async, [this, &task, agent_name]() {
                return run_one(task, agent_name);
            }));
        }

        for (auto& future : futures) {
            result.steps.push_back(future.get());
        }
    } else {
        for (const auto& agent_name : normalized_agent_names) {
            result.steps.push_back(run_one(task, agent_name));
        }
    }

    std::size_t success_count = 0;
    for (const auto& step : result.steps) {
        if (step.success) {
            success_count += 1;
        }
        audit_logger_.record_step(task.task_id, step);
    }

    result.success = success_count == result.steps.size() && !result.steps.empty();
    result.summary = BuildSummary(success_count, result.steps.size());
    result.output_json = BuildOutputJson(normalized_agent_names, success_count, result.steps.size());
    result.duration_ms = ElapsedMs(started_at);
    if (!result.success) {
        result.error_code = "SubagentFailure";
        result.error_message = "One or more subagents failed or were denied by policy.";
    }

    audit_logger_.record_task_end(task.task_id, result);
    memory_manager_.record_task(task, result);
    return result;
}

TaskStepRecord SubagentManager::run_one(const TaskRequest& task, const std::string& agent_name) const {
    const auto started_at = std::chrono::steady_clock::now();
    const auto agent = agent_registry_.find(agent_name);
    if (!agent || !agent->healthy()) {
        return {
            .target_kind = RouteTargetKind::agent,
            .target_name = agent_name,
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "Subagent was not found or is unhealthy.",
        };
    }

    AgentTask agent_task{
        .task_id = task.task_id + "." + agent_name,
        .task_type = task.task_type,
        .objective = task.objective,
        .workspace_path = task.workspace_path.string(),
        .context_json = "",
        .constraints_json = "",
        .timeout_ms = task.timeout_ms,
        .budget_limit = task.budget_limit,
    };

    const auto policy = policy_engine_.evaluate_agent(task, agent->profile(), agent_task);
    audit_logger_.record_policy(task.task_id, agent_name, policy);
    if (!policy.allowed) {
        return {
            .target_kind = RouteTargetKind::agent,
            .target_name = agent_name,
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "PolicyDenied",
            .error_message = policy.reason,
        };
    }

    const auto agent_result = agent->run_task(agent_task);
    return {
        .target_kind = RouteTargetKind::agent,
        .target_name = agent_name,
        .success = agent_result.success,
        .duration_ms = agent_result.duration_ms > 0 ? agent_result.duration_ms : ElapsedMs(started_at),
        .summary = agent_result.summary,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    };
}

}  // namespace agentos
