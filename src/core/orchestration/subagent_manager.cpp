#include "core/orchestration/subagent_manager.hpp"

#include "core/execution/task_lifecycle.hpp"
#include "core/orchestration/agent_result_normalizer.hpp"
#include "memory/lesson_hints.hpp"
#include "utils/json_utils.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
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

std::vector<std::string> SplitCommaList(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::vector<std::string> SplitSemicolonList(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ';')) {
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::optional<std::string> InputValue(const TaskRequest& task, const std::string& key) {
    const auto found = task.inputs.find(key);
    if (found == task.inputs.end() || found->second.empty()) {
        return std::nullopt;
    }
    return found->second;
}

bool IsEnabledInput(const TaskRequest& task, const std::string& key) {
    const auto value = InputValue(task, key);
    return value.has_value() && (*value == "true" || *value == "1" || *value == "yes");
}

std::optional<std::string> SubtaskFromList(const TaskRequest& task, const std::string& agent_name, const std::string& role) {
    const auto subtasks = InputValue(task, "subtasks");
    if (!subtasks.has_value()) {
        return std::nullopt;
    }
    for (const auto& item : SplitSemicolonList(*subtasks)) {
        const auto separator = item.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const auto key = item.substr(0, separator);
        const auto value = item.substr(separator + 1);
        if (!value.empty() && (key == agent_name || key == role)) {
            return value;
        }
    }
    return std::nullopt;
}

std::string SubtaskObjectiveFor(const TaskRequest& task, const std::string& agent_name, const std::string& role) {
    if (const auto value = InputValue(task, "subtask_" + agent_name); value.has_value()) {
        return *value;
    }
    if (const auto value = InputValue(task, "subtask." + agent_name); value.has_value()) {
        return *value;
    }
    if (const auto value = InputValue(task, "subtask_" + role); value.has_value()) {
        return *value;
    }
    if (const auto value = InputValue(task, "subtask." + role); value.has_value()) {
        return *value;
    }
    if (const auto value = SubtaskFromList(task, agent_name, role); value.has_value()) {
        return *value;
    }
    return task.objective;
}

std::string JsonUnescape(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1 >= value.size()) {
            output.push_back(value[index]);
            continue;
        }

        const auto escaped = value[++index];
        switch (escaped) {
        case '"':
            output.push_back('"');
            break;
        case '\\':
            output.push_back('\\');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        default:
            output.push_back(escaped);
            break;
        }
    }
    return output;
}

std::vector<std::string> ExtractPlanActions(const std::string& structured_output_json) {
    std::vector<std::string> actions;
    constexpr std::string_view key = R"("action")";
    std::size_t cursor = 0;
    while ((cursor = structured_output_json.find(key, cursor)) != std::string::npos) {
        const auto colon = structured_output_json.find(':', cursor + key.size());
        if (colon == std::string::npos) {
            break;
        }
        const auto quote = structured_output_json.find('"', colon + 1);
        if (quote == std::string::npos) {
            break;
        }

        std::string value;
        bool escaped = false;
        for (std::size_t index = quote + 1; index < structured_output_json.size(); ++index) {
            const auto ch = structured_output_json[index];
            if (!escaped && ch == '"') {
                if (!value.empty()) {
                    actions.push_back(JsonUnescape(value));
                }
                cursor = index + 1;
                break;
            }
            value.push_back(ch);
            escaped = !escaped && ch == '\\';
            if (ch != '\\') {
                escaped = false;
            }
        }
    }
    return actions;
}

std::string BuildSubtasksFromPlanActions(
    const std::vector<std::string>& roles,
    const std::vector<std::string>& actions,
    const std::string& fallback_objective) {
    std::ostringstream output;
    for (std::size_t index = 0; index < roles.size(); ++index) {
        if (index != 0) {
            output << ';';
        }
        const auto action = index < actions.size() ? actions[index] : fallback_objective;
        output << roles[index] << '=' << action;
    }
    return output.str();
}

std::optional<std::string> SelectDecompositionAgent(
    const TaskRequest& task,
    const AgentRegistry& agent_registry) {
    if (const auto configured = InputValue(task, "decomposition_agent"); configured.has_value()) {
        return *configured;
    }
    if (const auto configured = InputValue(task, "planner_agent"); configured.has_value()) {
        return *configured;
    }

    std::optional<std::string> selected;
    int best_score = std::numeric_limits<int>::min();
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto agent = agent_registry.find(profile.agent_name);
        if (!agent || !agent->healthy()) {
            continue;
        }
        int score = 0;
        for (const auto& capability : profile.capabilities) {
            if (capability.name == "decomposition") {
                score = std::max(score, capability.score);
            }
        }
        if (score > best_score) {
            best_score = score;
            selected = profile.agent_name;
        }
    }
    if (best_score <= 0) {
        return std::nullopt;
    }
    return selected;
}

std::string BuildSummary(const std::size_t success_count, const std::size_t total_count) {
    return "Subagent orchestration completed: " + std::to_string(success_count) + "/" +
           std::to_string(total_count) + " succeeded.";
}

bool IsJsonObjectLike(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    return first != std::string::npos && last != std::string::npos && value[first] == '{' && value[last] == '}';
}

std::string BuildAgentOutputsJson(const std::vector<TaskStepRecord>& steps) {
    std::ostringstream output;
    output << '[';
    bool first = true;
    for (const auto& step : steps) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << MakeJsonObject({
            {"agent", QuoteJson(step.target_name)},
            {"success", BoolAsJson(step.success)},
            {"summary", QuoteJson(step.summary)},
            {"normalized", BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
                .agent_name = step.target_name,
                .success = step.success,
                .summary = step.summary,
                .structured_output_json = step.structured_output_json,
                .artifacts = step.artifacts,
                .duration_ms = step.duration_ms,
                .estimated_cost = step.estimated_cost,
                .error_code = step.error_code,
                .error_message = step.error_message,
            })},
            {"output", IsJsonObjectLike(step.structured_output_json) ? step.structured_output_json : QuoteJson(step.structured_output_json)},
        });
    }
    output << ']';
    return output.str();
}

std::string BuildOutputJson(
    const std::vector<std::string>& agent_names,
    const std::vector<std::string>& roles,
    const std::vector<TaskStepRecord>& steps,
    const std::size_t success_count,
    const std::size_t total_count,
    const double estimated_cost) {
    return MakeJsonObject({
        {"agents", QuoteJson(JoinAgentNames(agent_names))},
        {"roles", QuoteJson(JoinAgentNames(roles))},
        {"success_count", NumberAsJson(static_cast<int>(success_count))},
        {"total_count", NumberAsJson(static_cast<int>(total_count))},
        {"estimated_cost", NumberAsJson(estimated_cost)},
        {"agent_outputs", BuildAgentOutputsJson(steps)},
    });
}

int LessonOccurrenceCount(
    const MemoryManager& memory_manager,
    const std::string& task_type,
    const std::string& target_name) {
    int occurrences = 0;
    for (const auto& lesson : memory_manager.lesson_store().list()) {
        if (lesson.enabled && lesson.task_type == task_type && lesson.target_name == target_name) {
            occurrences += lesson.occurrence_count;
        }
    }
    return occurrences;
}

bool HasCapabilityForTask(const AgentProfile& profile, const std::string& task_type) {
    return std::any_of(profile.capabilities.begin(), profile.capabilities.end(), [&](const AgentCapability& capability) {
        return capability.name == task_type;
    });
}

int CapabilityScoreForTask(const AgentProfile& profile, const std::string& task_type) {
    int score = 0;
    for (const auto& capability : profile.capabilities) {
        if (capability.name == task_type) {
            score = std::max(score, capability.score);
        }
    }
    return score;
}

std::string RoleFromProfile(const AgentProfile& profile, const std::string& task_type) {
    for (const auto& capability : profile.capabilities) {
        if (capability.name == task_type) {
            return capability.name;
        }
    }
    if (!profile.capabilities.empty()) {
        return profile.capabilities.front().name;
    }
    return "contributor";
}

std::vector<std::string> BuildSubagentRoles(
    const TaskRequest& task,
    const std::vector<std::string>& agent_names,
    const AgentRegistry& agent_registry) {
    std::vector<std::string> roles(agent_names.size(), "contributor");
    const auto roles_input = task.inputs.find("roles");
    if (roles_input != task.inputs.end()) {
        const auto configured_roles = SplitCommaList(roles_input->second);
        std::size_t positional_index = 0;
        for (const auto& configured_role : configured_roles) {
            const auto separator = configured_role.find(':');
            if (separator != std::string::npos) {
                const auto agent_name = configured_role.substr(0, separator);
                const auto role = configured_role.substr(separator + 1);
                const auto found = std::find(agent_names.begin(), agent_names.end(), agent_name);
                if (found != agent_names.end() && !role.empty()) {
                    roles[static_cast<std::size_t>(std::distance(agent_names.begin(), found))] = role;
                }
                continue;
            }
            if (positional_index < roles.size()) {
                roles[positional_index] = configured_role;
                positional_index += 1;
            }
        }
    }

    for (std::size_t index = 0; index < agent_names.size(); ++index) {
        if (roles[index] != "contributor") {
            continue;
        }
        const auto agent = agent_registry.find(agent_names[index]);
        if (agent) {
            roles[index] = RoleFromProfile(agent->profile(), task.task_type);
        }
    }

    return roles;
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
    const std::size_t max_subagents,
    const std::size_t max_parallel_subagents,
    const double max_estimated_cost)
    : agent_registry_(agent_registry),
      policy_engine_(policy_engine),
      audit_logger_(audit_logger),
      memory_manager_(memory_manager),
      max_subagents_(max_subagents),
      max_parallel_subagents_(max_parallel_subagents),
      max_estimated_cost_(max_estimated_cost) {}

TaskRunResult SubagentManager::run(
    const TaskRequest& task,
    const std::vector<std::string>& agent_names,
    const SubagentExecutionMode mode) {
    const auto started_at = std::chrono::steady_clock::now();
    const bool automatic_selection = agent_names.empty();
    const auto normalized_agent_names = automatic_selection
        ? select_agent_candidates(task)
        : NormalizeAgentNames(agent_names);
    const auto subagent_roles = BuildSubagentRoles(task, normalized_agent_names, agent_registry_);
    TaskRequest effective_task = task;
    std::string decomposition_agent_name;

    audit_logger_.record_task_start(task);
    audit_logger_.record_route(task, RouteDecision{
        .target_kind = RouteTargetKind::agent,
        .target_name = "subagents",
        .rationale = std::string(automatic_selection ? "automatic" : "explicit") +
            " subagent orchestration in " + ToString(mode) + " mode",
    });

    TaskRunResult result{
        .route_target = "subagents",
        .route_kind = RouteTargetKind::agent,
    };

    if (normalized_agent_names.empty()) {
        result.success = false;
        result.summary = "No subagents were selected.";
        result.error_code = "SubagentRouteNotFound";
        result.error_message = automatic_selection
            ? "no healthy agent candidates matched subagent orchestration."
            : "agents=<name[,name]> is required for explicit subagent orchestration.";
        result.duration_ms = ElapsedMs(started_at);
        FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
        return result;
    }

    if (IsEnabledInput(task, "auto_decompose") && !InputValue(task, "subtasks").has_value()) {
        const auto decomposition_agent = SelectDecompositionAgent(task, agent_registry_);
        if (!decomposition_agent.has_value()) {
            result.success = false;
            result.summary = "No decomposition agent was selected.";
            result.error_code = "DecompositionAgentNotFound";
            result.error_message = "auto_decompose=true requires a healthy agent with decomposition capability.";
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }

        const auto planner = agent_registry_.find(*decomposition_agent);
        if (!planner || !planner->healthy()) {
            result.success = false;
            result.summary = "Decomposition agent is unavailable.";
            result.error_code = "DecompositionAgentUnavailable";
            result.error_message = "selected decomposition agent was not found or is unhealthy.";
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }

        decomposition_agent_name = *decomposition_agent;
        AgentTask decomposition_task{
            .task_id = task.task_id + ".decomposition",
            .task_type = "decomposition",
            .objective = task.objective,
            .workspace_path = task.workspace_path.string(),
            .context_json = MakeJsonObject({
                {"parent_task_id", QuoteJson(task.task_id)},
                {"agents", QuoteJson(JoinAgentNames(normalized_agent_names))},
                {"roles", QuoteJson(JoinAgentNames(subagent_roles))},
            }),
            .timeout_ms = task.timeout_ms,
            .budget_limit = task.budget_limit,
        };

        auto policy = policy_engine_.evaluate_agent(task, planner->profile(), decomposition_task);
        ApplyLessonPolicyHint(memory_manager_, task, decomposition_agent_name, policy);
        audit_logger_.record_policy(task.task_id, decomposition_agent_name, policy);
        if (!policy.allowed) {
            result.success = false;
            result.summary = "Decomposition agent was denied by policy.";
            result.error_code = "PolicyDenied";
            result.error_message = policy.reason;
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }

        const auto decomposition = planner->run_task(decomposition_task);
        if (!decomposition.success) {
            result.success = false;
            result.summary = "Decomposition agent failed.";
            result.error_code = decomposition.error_code.empty() ? "DecompositionFailed" : decomposition.error_code;
            result.error_message = decomposition.error_message;
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }

        const auto actions = ExtractPlanActions(decomposition.structured_output_json);
        if (actions.empty()) {
            result.success = false;
            result.summary = "Decomposition agent did not return plan actions.";
            result.error_code = "DecompositionOutputInvalid";
            result.error_message = "structured output must include plan_steps[].action values.";
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }
        effective_task.inputs["subtasks"] = BuildSubtasksFromPlanActions(subagent_roles, actions, task.objective);
    }

    if (normalized_agent_names.size() > max_subagents_) {
        result.success = false;
        result.summary = "Too many subagents were requested.";
        result.error_code = "TooManySubagents";
        result.error_message = "Requested " + std::to_string(normalized_agent_names.size()) +
                               " subagents; max is " + std::to_string(max_subagents_) + ".";
        result.duration_ms = ElapsedMs(started_at);
        FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
        return result;
    }

    if (mode == SubagentExecutionMode::parallel && normalized_agent_names.size() > max_parallel_subagents_) {
        result.success = false;
        result.summary = "Too many parallel subagents were requested.";
        result.error_code = "TooManyParallelSubagents";
        result.error_message = "Requested " + std::to_string(normalized_agent_names.size()) +
                               " parallel subagents; max is " + std::to_string(max_parallel_subagents_) + ".";
        result.duration_ms = ElapsedMs(started_at);
        FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
        return result;
    }

    if (mode == SubagentExecutionMode::parallel) {
        std::vector<std::future<TaskStepRecord>> futures;
        futures.reserve(normalized_agent_names.size());
        for (std::size_t index = 0; index < normalized_agent_names.size(); ++index) {
            const auto agent_name = normalized_agent_names[index];
            const auto role = subagent_roles[index];
            futures.push_back(std::async(std::launch::async, [this, &effective_task, agent_name, role]() {
                return run_one(effective_task, agent_name, role);
            }));
        }

        for (auto& future : futures) {
            result.steps.push_back(future.get());
        }
    } else {
        for (std::size_t index = 0; index < normalized_agent_names.size(); ++index) {
            result.steps.push_back(run_one(effective_task, normalized_agent_names[index], subagent_roles[index]));
        }
    }

    std::size_t success_count = 0;
    double estimated_cost = 0.0;
    for (const auto& step : result.steps) {
        if (step.success) {
            success_count += 1;
        }
        estimated_cost += step.estimated_cost;
        RecordTaskStep(audit_logger_, task.task_id, step);
    }

    result.success = success_count == result.steps.size() && !result.steps.empty();
    result.summary = BuildSummary(success_count, result.steps.size());
    result.output_json = BuildOutputJson(
        normalized_agent_names,
        subagent_roles,
        result.steps,
        success_count,
        result.steps.size(),
        estimated_cost);
    if (!decomposition_agent_name.empty()) {
        result.output_json.pop_back();
        result.output_json += ",\"decomposition_agent\":" + QuoteJson(decomposition_agent_name) + '}';
    }
    result.duration_ms = ElapsedMs(started_at);
    const auto effective_cost_limit = task.budget_limit > 0.0 ? task.budget_limit : max_estimated_cost_;
    if (effective_cost_limit > 0.0 && estimated_cost > effective_cost_limit) {
        result.success = false;
        result.error_code = "SubagentCostLimitExceeded";
        result.error_message = "Estimated subagent cost " + NumberAsJson(estimated_cost) +
                               " exceeded limit " + NumberAsJson(effective_cost_limit) + ".";
    }
    if (!result.success) {
        if (result.error_code.empty()) {
            result.error_code = "SubagentFailure";
            result.error_message = "One or more subagents failed or were denied by policy.";
        }
    }

    FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
    return result;
}

std::vector<std::string> SubagentManager::select_agent_candidates(const TaskRequest& task) const {
    struct Candidate {
        std::string name;
        bool capability_matched = false;
        double score = -std::numeric_limits<double>::infinity();
    };

    std::vector<Candidate> candidates;
    for (const auto& profile : agent_registry_.list_profiles()) {
        const auto agent = agent_registry_.find(profile.agent_name);
        if (!agent || !agent->healthy()) {
            continue;
        }

        const bool capability_matched = HasCapabilityForTask(profile, task.task_type);
        double score = static_cast<double>(CapabilityScoreForTask(profile, task.task_type));
        if (const auto stats_it = memory_manager_.agent_stats().find(profile.agent_name);
            stats_it != memory_manager_.agent_stats().end() && stats_it->second.total_runs > 0) {
            const auto& stats = stats_it->second;
            const auto success_rate = static_cast<double>(stats.success_runs) / static_cast<double>(stats.total_runs);
            score += (success_rate * 100.0) - (stats.avg_duration_ms / 1000.0);
        }
        score -= static_cast<double>(LessonOccurrenceCount(memory_manager_, task.task_type, profile.agent_name)) * 25.0;

        candidates.push_back(Candidate{
            .name = profile.agent_name,
            .capability_matched = capability_matched,
            .score = score,
        });
    }

    const bool has_capability_match = std::any_of(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
        return candidate.capability_matched;
    });
    if (has_capability_match) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const Candidate& candidate) {
            return !candidate.capability_matched;
        }), candidates.end());
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.name < right.name;
    });

    std::vector<std::string> selected;
    selected.reserve(std::min(max_subagents_, candidates.size()));
    for (const auto& candidate : candidates) {
        if (selected.size() >= max_subagents_) {
            break;
        }
        selected.push_back(candidate.name);
    }
    return selected;
}

TaskStepRecord SubagentManager::run_one(const TaskRequest& task, const std::string& agent_name, const std::string& role) const {
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

    const auto subtask_objective = SubtaskObjectiveFor(task, agent_name, role);
    AgentTask agent_task{
        .task_id = task.task_id + "." + agent_name,
        .task_type = task.task_type,
        .objective = "[" + role + "] " + subtask_objective,
        .workspace_path = task.workspace_path.string(),
        .context_json = MakeJsonObject({
            {"parent_task_id", QuoteJson(task.task_id)},
            {"agent", QuoteJson(agent_name)},
            {"role", QuoteJson(role)},
            {"original_objective", QuoteJson(task.objective)},
            {"subtask_objective", QuoteJson(subtask_objective)},
        }),
        .constraints_json = "",
        .timeout_ms = task.timeout_ms,
        .budget_limit = task.budget_limit,
    };

    auto policy = policy_engine_.evaluate_agent(task, agent->profile(), agent_task);
    ApplyLessonPolicyHint(memory_manager_, task, agent_name, policy);
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
        .estimated_cost = agent_result.estimated_cost,
        .summary = agent_result.summary,
        .structured_output_json = agent_result.structured_output_json,
        .artifacts = agent_result.artifacts,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    };
}

}  // namespace agentos
