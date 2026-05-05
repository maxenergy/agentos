#include "core/orchestration/subagent_manager.hpp"

#include "core/execution/task_lifecycle.hpp"
#include "core/orchestration/agent_dispatch.hpp"
#include "core/orchestration/agent_result_normalizer.hpp"
#include "memory/lesson_hints.hpp"
#include "utils/cancellation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

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

// Strictly typed walk: substring search would let `{"summary":"action: x"}`
// inject "x" as a subtask, so we require plan_steps to be an array of objects
// with string `action` fields. The legacy `agent_result.v1` envelope nests the
// raw planner payload under `raw_output`, so we accept either location.
std::vector<std::string> ExtractPlanActionsFromArray(const nlohmann::json& steps) {
    std::vector<std::string> actions;
    if (!steps.is_array()) {
        return {};
    }
    actions.reserve(steps.size());
    for (const auto& element : steps) {
        if (!element.is_object()) {
            return {};
        }
        const auto action_it = element.find("action");
        if (action_it == element.end() || !action_it->is_string()) {
            return {};
        }
        actions.push_back(action_it->get<std::string>());
    }
    return actions;
}

std::vector<std::string> ExtractPlanActions(
    const std::string& structured_output_json,
    AuditLogger& audit_logger,
    const std::string& task_id) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(structured_output_json);
    } catch (const nlohmann::json::parse_error& error) {
        audit_logger.record_config_diagnostic(
            "subagent_decomposition", task_id, 0,
            std::string("plan parse error: ") + error.what());
        return {};
    }

    if (!root.is_object()) {
        audit_logger.record_config_diagnostic(
            "subagent_decomposition", task_id, 0, "plan root must be an object");
        return {};
    }

    const auto walk = [](const nlohmann::json& container) -> std::vector<std::string> {
        const auto it = container.find("plan_steps");
        if (it == container.end()) {
            return {};
        }
        return ExtractPlanActionsFromArray(*it);
    };

    auto actions = walk(root);
    if (!actions.empty()) {
        return actions;
    }

    if (const auto raw_it = root.find("raw_output"); raw_it != root.end() && raw_it->is_object()) {
        actions = walk(*raw_it);
        if (!actions.empty()) {
            return actions;
        }
    }

    audit_logger.record_config_diagnostic(
        "subagent_decomposition", task_id, 0,
        "plan_steps must be an array of objects with string action fields");
    return {};
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

std::optional<nlohmann::ordered_json> ParseObjectJson(const std::string& value) {
    try {
        auto parsed = nlohmann::ordered_json::parse(value);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

nlohmann::ordered_json EmbeddedJsonOrString(const std::string& value) {
    if (auto parsed = ParseObjectJson(value); parsed.has_value()) {
        return *parsed;
    }
    return value;
}

nlohmann::ordered_json BuildAgentOutputsJson(const std::vector<TaskStepRecord>& steps) {
    auto outputs = nlohmann::ordered_json::array();
    for (const auto& step : steps) {
        nlohmann::ordered_json entry;
        entry["agent"] = step.target_name;
        entry["success"] = step.success;
        entry["summary"] = step.summary;
        entry["normalized"] = EmbeddedJsonOrString(BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
            .agent_name = step.target_name,
            .success = step.success,
            .summary = step.summary,
            .structured_output_json = step.structured_output_json,
            .artifacts = step.artifacts,
            .duration_ms = step.duration_ms,
            .estimated_cost = step.estimated_cost,
            .error_code = step.error_code,
            .error_message = step.error_message,
        }));
        entry["output"] = EmbeddedJsonOrString(step.structured_output_json);
        outputs.push_back(std::move(entry));
    }
    return outputs;
}

std::string BuildOutputJson(
    const std::vector<std::string>& agent_names,
    const std::vector<std::string>& roles,
    const std::vector<TaskStepRecord>& steps,
    const std::size_t success_count,
    const std::size_t total_count,
    const double estimated_cost,
    const std::string& decomposition_agent_name) {
    nlohmann::ordered_json output;
    output["agents"] = JoinAgentNames(agent_names);
    output["roles"] = JoinAgentNames(roles);
    output["success_count"] = success_count;
    output["total_count"] = total_count;
    output["estimated_cost"] = estimated_cost;
    output["agent_outputs"] = BuildAgentOutputsJson(steps);
    if (!decomposition_agent_name.empty()) {
        output["decomposition_agent"] = decomposition_agent_name;
    }
    return output.dump();
}

std::string DecompositionContextJson(
    const TaskRequest& task,
    const std::vector<std::string>& agent_names,
    const std::vector<std::string>& roles) {
    nlohmann::ordered_json context;
    context["parent_task_id"] = task.task_id;
    context["agents"] = JoinAgentNames(agent_names);
    context["roles"] = JoinAgentNames(roles);
    return context.dump();
}

std::string SubagentContextJson(
    const TaskRequest& task,
    const std::string& agent_name,
    const std::string& role,
    const std::string& subtask_objective) {
    nlohmann::ordered_json context;
    context["parent_task_id"] = task.task_id;
    context["agent"] = agent_name;
    context["role"] = role;
    context["original_objective"] = task.objective;
    context["subtask_objective"] = subtask_objective;
    return context.dump();
}

std::string JsonNumber(const double value) {
    return nlohmann::ordered_json(value).dump();
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
    const SubagentExecutionMode mode,
    std::shared_ptr<CancellationToken> cancel) {
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
        TaskRequest decomposition_request = task;
        decomposition_request.task_type = "decomposition";
        auto decomposition_dispatch = DispatchAgent(
            AgentDispatchInput{
                .task = decomposition_request,
                .agent = planner,
                .agent_name = decomposition_agent_name,
                .agent_task_id = task.task_id + ".decomposition",
                .objective = task.objective,
                .context_json = DecompositionContextJson(task, normalized_agent_names, subagent_roles),
                .constraints_json = "",
                .invocation_context = {
                    {"task_type", "decomposition"},
                    {"parent_task_id", task.task_id},
                    {"agent", decomposition_agent_name},
                    {"subagents", JoinAgentNames(normalized_agent_names)},
                    {"roles", JoinAgentNames(subagent_roles)},
                },
                .cancel = cancel,
            },
            policy_engine_,
            audit_logger_,
            memory_manager_);
        if (decomposition_dispatch.error_code == "PolicyDenied") {
            result.success = false;
            result.summary = "Decomposition agent was denied by policy.";
            result.error_code = "PolicyDenied";
            result.error_message = decomposition_dispatch.error_message;
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }
        if (!decomposition_dispatch.success) {
            result.success = false;
            result.summary = "Decomposition agent failed.";
            result.error_code = decomposition_dispatch.error_code.empty()
                ? "DecompositionFailed"
                : decomposition_dispatch.error_code;
            result.error_message = decomposition_dispatch.error_message;
            result.duration_ms = ElapsedMs(started_at);
            FinalizeTaskRun(audit_logger_, memory_manager_, task, result);
            return result;
        }

        const auto actions = ExtractPlanActions(
            decomposition_dispatch.structured_output_json,
            audit_logger_,
            task.task_id);
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
            futures.push_back(std::async(std::launch::async, [this, &effective_task, agent_name, role, cancel]() {
                return run_one(effective_task, agent_name, role, cancel);
            }));
        }

        for (auto& future : futures) {
            result.steps.push_back(future.get());
        }
    } else {
        for (std::size_t index = 0; index < normalized_agent_names.size(); ++index) {
            // Sequential mode short-circuits as soon as the orchestrator
            // cancels: subsequent agents record a Cancelled step instead of
            // dispatching. (Parallel mode already started every future before
            // we observe the cancel.)
            if (cancel && cancel->is_cancelled()) {
                result.steps.push_back(TaskStepRecord{
                    .target_kind = RouteTargetKind::agent,
                    .target_name = normalized_agent_names[index],
                    .success = false,
                    .duration_ms = 0,
                    .error_code = "Cancelled",
                    .error_message = "subagent dispatch was cancelled by the orchestrator",
                });
                continue;
            }
            result.steps.push_back(run_one(effective_task, normalized_agent_names[index], subagent_roles[index], cancel));
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
        estimated_cost,
        decomposition_agent_name);
    result.duration_ms = ElapsedMs(started_at);
    const auto effective_cost_limit = task.budget_limit > 0.0 ? task.budget_limit : max_estimated_cost_;
    if (effective_cost_limit > 0.0 && estimated_cost > effective_cost_limit) {
        result.success = false;
        result.error_code = "SubagentCostLimitExceeded";
        result.error_message = "Estimated subagent cost " + JsonNumber(estimated_cost) +
                               " exceeded limit " + JsonNumber(effective_cost_limit) + ".";
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

TaskStepRecord SubagentManager::run_one(
    const TaskRequest& task,
    const std::string& agent_name,
    const std::string& role,
    const std::shared_ptr<CancellationToken>& cancel) const {
    const auto agent = agent_registry_.find(agent_name);
    const auto subtask_objective = SubtaskObjectiveFor(task, agent_name, role);
    auto dispatch_result = DispatchAgent(
        AgentDispatchInput{
            .task = task,
            .agent = agent,
            .agent_name = agent_name,
            .agent_task_id = task.task_id + "." + agent_name,
            .objective = "[" + role + "] " + subtask_objective,
            .context_json = SubagentContextJson(task, agent_name, role, subtask_objective),
            .invocation_context = {
                {"task_type", task.task_type},
                {"parent_task_id", task.task_id},
                {"agent", agent_name},
                {"role", role},
                {"original_objective", task.objective},
                {"subtask_objective", subtask_objective},
            },
            .cancel = cancel,
        },
        policy_engine_,
        audit_logger_,
        memory_manager_);

    auto step = std::move(dispatch_result.step);
    if (step.error_code == "Cancelled" && step.error_message == "agent dispatch was cancelled by the orchestrator") {
        step.error_message = "subagent dispatch was cancelled by the orchestrator";
    } else if (step.error_code == "AgentUnavailable" && step.error_message == "Agent was not found or is unhealthy.") {
        step.error_message = "Subagent was not found or is unhealthy.";
    }
    return step;
}

}  // namespace agentos
