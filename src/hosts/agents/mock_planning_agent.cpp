#include "hosts/agents/mock_planning_agent.hpp"

#include "utils/json_utils.hpp"

#include <chrono>

namespace agentos {

AgentProfile MockPlanningAgent::profile() const {
    return {
        .agent_name = "mock_planner",
        .version = "0.1.0",
        .description = "Deterministic planning adapter used to validate the agent loop before a real provider is integrated.",
        .capabilities = {
            {"planning", 90},
            {"analysis", 80},
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

bool MockPlanningAgent::healthy() const {
    return true;
}

std::string MockPlanningAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "mock-session-" + std::to_string(next_id);
}

void MockPlanningAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult MockPlanningAgent::run_task(const AgentTask& task) {
    const auto started_at = std::chrono::steady_clock::now();

    return {
        .success = true,
        .summary = "Generated a deterministic execution plan for: " + task.objective,
        .structured_output_json = MakeJsonObject({
            {"task_type", QuoteJson(task.task_type)},
            {"objective", QuoteJson(task.objective)},
            {"step_1", QuoteJson("inspect the current workspace state")},
            {"step_2", QuoteJson("select the narrowest safe execution path")},
            {"step_3", QuoteJson("execute and record structured audit logs")},
        }),
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count()),
        .estimated_cost = 0.0,
    };
}

AgentResult MockPlanningAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    result.summary = "[" + session_id + "] " + result.summary;
    return result;
}

bool MockPlanningAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

}  // namespace agentos

