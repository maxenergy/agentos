#include "hosts/agents/local_planning_agent.hpp"

#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/cancellation.hpp"

#include <chrono>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

// Stable error code for cooperative cancellation; documented in the V2 smoke
// test (TestLocalPlanningAgentV2Cancels).
constexpr const char* kCancelledErrorCode = "Cancelled";

AgentResult MakeCancelledResult(int duration_ms) {
    return {
        .success = false,
        .duration_ms = duration_ms,
        .error_code = kCancelledErrorCode,
        .error_message = "local planning task was cancelled",
    };
}

bool EmitEvent(const AgentEventCallback& on_event, AgentEvent event) {
    if (!on_event) {
        return true;
    }
    return on_event(event);
}

}  // namespace

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

std::string LocalPlanningAgent::allocate_session_id() {
    const auto next_id = session_counter_.fetch_add(1) + 1;
    auto session_id = "local-session-" + std::to_string(next_id);
    {
        std::lock_guard lock(mutex_);
        active_sessions_.insert(session_id);
    }
    return session_id;
}

std::string LocalPlanningAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    return allocate_session_id();
}

std::optional<std::string> LocalPlanningAgent::open_session(const StringMap& config) {
    (void)config;
    return allocate_session_id();
}

void LocalPlanningAgent::close_session(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    active_sessions_.erase(session_id);
}

AgentResult LocalPlanningAgent::invoke(const AgentInvocation& invocation,
                                       const AgentEventCallback& on_event) {
    const auto started_at = std::chrono::steady_clock::now();

    const auto cancelled = [&]() {
        return invocation.cancel && invocation.cancel->is_cancelled();
    };
    const auto elapsed_ms = [&]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count());
    };

    if (cancelled()) {
        return MakeCancelledResult(elapsed_ms());
    }

    // SessionInit so a streaming caller knows the upstream id (if any).
    if (invocation.session_id.has_value()) {
        EmitEvent(on_event, AgentEvent{
            .kind = AgentEvent::Kind::SessionInit,
            .fields = {
                {"session_id", *invocation.session_id},
                {"agent", "local_planner"},
            },
        });
    }

    EmitEvent(on_event, AgentEvent{
        .kind = AgentEvent::Kind::Status,
        .fields = {{"phase", "planning"}},
        .payload_text = "planning local execution steps",
    });

    // task_type signal still drives the canned plan shape; lifted from the
    // legacy run_task() body verbatim so existing fixtures keep matching.
    const auto task_type_it = invocation.context.find("task_type");
    const std::string task_type = task_type_it == invocation.context.end() ? std::string{} : task_type_it->second;

    std::vector<std::string> steps{
        "Clarify the requested outcome and constraints",
        "Inspect relevant workspace state before changing files",
        "Choose the narrowest implementation path that satisfies the request",
        "Run focused verification for changed behavior",
        "Record resulting state, risks, and follow-up work",
    };
    if (task_type.find("analysis") != std::string::npos || task_type.find("plan") != std::string::npos) {
        steps = {
            "Extract decision points from the objective",
            "Map required evidence to local files, commands, or provider outputs",
            "Compare viable approaches against policy, cost, and reversibility",
            "Return a concrete ordered plan with explicit verification",
        };
    } else if (task_type.find("write") != std::string::npos || task_type.find("patch") != std::string::npos) {
        steps = {
            "Locate the smallest owned code or document surface",
            "Apply the requested change while preserving existing user edits",
            "Validate formatting, policy checks, and behavior near the edit",
            "Summarize changed files and verification results",
        };
    }

    if (cancelled()) {
        return MakeCancelledResult(elapsed_ms());
    }

    nlohmann::json step_array = nlohmann::json::array();
    for (std::size_t index = 0; index < steps.size(); ++index) {
        nlohmann::json step;
        step["order"] = static_cast<int>(index + 1);
        step["action"] = steps[index];
        step_array.push_back(std::move(step));
    }

    const auto summary = "Created local execution plan with " + std::to_string(steps.size()) + " steps for: " + invocation.objective;

    nlohmann::json legacy_output_json;
    legacy_output_json["content"] = summary;
    legacy_output_json["agent"] = "local_planner";
    legacy_output_json["provider"] = "local_planner";
    legacy_output_json["task_type"] = task_type;
    legacy_output_json["objective"] = invocation.objective;
    legacy_output_json["plan_steps"] = std::move(step_array);
    const auto legacy_output = legacy_output_json.dump();

    AgentResult agent_result{
        .success = true,
        .summary = summary,
        .structured_output_json = legacy_output,
        .duration_ms = elapsed_ms(),
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
    // This adapter is offline / free; record a zero-cost Usage so the V2
    // budget gate sees an explicit zero rather than missing data.
    agent_result.usage.cost_usd = 0.0;
    agent_result.usage.turns = 1;

    EmitEvent(on_event, AgentEvent{
        .kind = AgentEvent::Kind::Usage,
        .fields = {
            {"cost_usd", "0.0"},
            {"input_tokens", "0"},
            {"output_tokens", "0"},
            {"turns", "1"},
        },
    });
    EmitEvent(on_event, AgentEvent{
        .kind = AgentEvent::Kind::Final,
        .fields = {{"success", "true"}},
        .payload_text = summary,
    });

    return agent_result;
}

AgentResult LocalPlanningAgent::run_task(const AgentTask& task) {
    AgentInvocation invocation{
        .task_id = task.task_id,
        .objective = task.objective,
        .workspace_path = task.workspace_path,
        .context = {{"task_type", task.task_type}},
        .timeout_ms = task.timeout_ms,
        .budget_limit_usd = task.budget_limit,
    };
    return invoke(invocation);
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
    if (result.success) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool LocalPlanningAgent::cancel(const std::string& /*task_id*/) {
    // Phase 4.1: legacy cancel(task_id) is a no-op stub. Cancellation is now
    // driven via AgentInvocation::cancel (CancellationToken). No callers in
    // the live codebase, so returning false is safe.
    return false;
}

}  // namespace agentos
