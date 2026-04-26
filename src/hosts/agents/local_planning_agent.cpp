#include "hosts/agents/local_planning_agent.hpp"

#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/json_utils.hpp"

#include <chrono>
#include <sstream>
#include <vector>

namespace agentos {

AgentProfile LocalPlanningAgent::profile() const {
    return {
        .agent_name = "local_planner",
        .version = "1.0.0",
        .description = "Local deterministic planning agent for offline analysis, task breakdown, and execution sequencing.",
        .capabilities = {
            {"planning", 90},
            {"analysis", 80},
            {"decomposition", 75},
        },
        .supports_session = true,
        .supports_streaming = false,
        .supports_patch = false,
        .supports_subagents = false,
        .supports_network = false,
        .cost_tier = "free",
        .latency_tier = "low",
        .risk_level = "low",
    };
}

bool LocalPlanningAgent::healthy() const {
    return true;
}

std::string LocalPlanningAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    const auto session_id = "local-session-" + std::to_string(next_id);
    {
        std::lock_guard lock(mutex_);
        active_sessions_.insert(session_id);
    }
    return session_id;
}

void LocalPlanningAgent::close_session(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    active_sessions_.erase(session_id);
}

AgentResult LocalPlanningAgent::run_task(const AgentTask& task) {
    const auto started_at = std::chrono::steady_clock::now();
    if (is_cancelled(task.task_id)) {
        return {
            .success = false,
            .duration_ms = 0,
            .error_code = "TaskCancelled",
            .error_message = "local planning task was cancelled before execution",
        };
    }

    std::vector<std::string> steps{
        "Clarify the requested outcome and constraints",
        "Inspect relevant workspace state before changing files",
        "Choose the narrowest implementation path that satisfies the request",
        "Run focused verification for changed behavior",
        "Record resulting state, risks, and follow-up work",
    };
    if (task.task_type.find("analysis") != std::string::npos || task.task_type.find("plan") != std::string::npos) {
        steps = {
            "Extract decision points from the objective",
            "Map required evidence to local files, commands, or provider outputs",
            "Compare viable approaches against policy, cost, and reversibility",
            "Return a concrete ordered plan with explicit verification",
        };
    } else if (task.task_type.find("write") != std::string::npos || task.task_type.find("patch") != std::string::npos) {
        steps = {
            "Locate the smallest owned code or document surface",
            "Apply the requested change while preserving existing user edits",
            "Validate formatting, policy checks, and behavior near the edit",
            "Summarize changed files and verification results",
        };
    }

    std::ostringstream step_json;
    step_json << '[';
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (index != 0) {
            step_json << ',';
        }
        step_json << MakeJsonObject({
            {"order", NumberAsJson(static_cast<int>(index + 1))},
            {"action", QuoteJson(steps[index])},
        });
    }
    step_json << ']';

    const auto summary = "Created local execution plan with " + std::to_string(steps.size()) + " steps for: " + task.objective;

    const auto legacy_output = MakeJsonObject({
        {"content", QuoteJson(summary)},
        {"agent", QuoteJson("local_planner")},
        {"provider", QuoteJson("local_planner")},
        {"task_type", QuoteJson(task.task_type)},
        {"objective", QuoteJson(task.objective)},
        {"plan_steps", step_json.str()},
    });
    AgentResult agent_result{
        .success = true,
        .summary = summary,
        .structured_output_json = legacy_output,
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count()),
        .estimated_cost = 0.0,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "local_planner",
        .success = agent_result.success,
        .summary = agent_result.summary,
        .structured_output_json = legacy_output,
        .artifacts = agent_result.artifacts,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = agent_result.estimated_cost,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    });
    return agent_result;
}

AgentResult LocalPlanningAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    {
        std::lock_guard lock(mutex_);
        if (!active_sessions_.contains(session_id)) {
            return {
                .success = false,
                .duration_ms = 0,
                .error_code = "SessionNotFound",
                .error_message = "local planning session is not open",
            };
        }
    }

    auto result = run_task(task);
    result.summary = "[" + session_id + "] " + result.summary;
    return result;
}

bool LocalPlanningAgent::cancel(const std::string& task_id) {
    if (task_id.empty()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    cancelled_tasks_.insert(task_id);
    return true;
}

bool LocalPlanningAgent::is_cancelled(const std::string& task_id) const {
    if (task_id.empty()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    return cancelled_tasks_.contains(task_id);
}

}  // namespace agentos
