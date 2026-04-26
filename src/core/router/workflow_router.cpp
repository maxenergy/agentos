#include "core/router/router_components.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <utility>

namespace agentos {

namespace {

constexpr int kRepeatedLessonThreshold = 2;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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

bool WorkflowInputEqualsSatisfied(const WorkflowDefinition& workflow, const TaskRequest& task) {
    return std::all_of(
        workflow.input_equals.begin(),
        workflow.input_equals.end(),
        [&](const std::string& condition) {
            const auto delimiter = condition.find('=');
            if (delimiter == std::string::npos || delimiter == 0) {
                return false;
            }

            const auto input_name = condition.substr(0, delimiter);
            const auto expected_value = condition.substr(delimiter + 1);
            const auto input = task.inputs.find(input_name);
            return input != task.inputs.end() && input->second == expected_value;
        });
}

std::optional<std::pair<std::string, double>> ParseNumericCondition(const std::string& condition) {
    const auto delimiter = condition.find('=');
    if (delimiter == std::string::npos || delimiter == 0) {
        return std::nullopt;
    }

    try {
        return std::make_pair(condition.substr(0, delimiter), std::stod(condition.substr(delimiter + 1)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> ParseTaskInputNumber(const TaskRequest& task, const std::string& input_name) {
    const auto input = task.inputs.find(input_name);
    if (input == task.inputs.end()) {
        return std::nullopt;
    }

    try {
        return std::stod(input->second);
    } catch (...) {
        return std::nullopt;
    }
}

bool WorkflowNumericConditionsSatisfied(const WorkflowDefinition& workflow, const TaskRequest& task) {
    const auto gte_satisfied = std::all_of(
        workflow.input_number_gte.begin(),
        workflow.input_number_gte.end(),
        [&](const std::string& condition) {
            const auto parsed_condition = ParseNumericCondition(condition);
            if (!parsed_condition.has_value()) {
                return false;
            }

            const auto input_value = ParseTaskInputNumber(task, parsed_condition->first);
            return input_value.has_value() && *input_value >= parsed_condition->second;
        });
    if (!gte_satisfied) {
        return false;
    }

    return std::all_of(
        workflow.input_number_lte.begin(),
        workflow.input_number_lte.end(),
        [&](const std::string& condition) {
            const auto parsed_condition = ParseNumericCondition(condition);
            if (!parsed_condition.has_value()) {
                return false;
            }

            const auto input_value = ParseTaskInputNumber(task, parsed_condition->first);
            return input_value.has_value() && *input_value <= parsed_condition->second;
        });
}

std::optional<bool> ParseBooleanValue(const std::string& value) {
    const auto lower = ToLower(value);
    if (lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "false" || lower == "0") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, bool>> ParseBooleanCondition(const std::string& condition) {
    const auto delimiter = condition.find('=');
    if (delimiter == std::string::npos || delimiter == 0) {
        return std::nullopt;
    }

    const auto expected_value = ParseBooleanValue(condition.substr(delimiter + 1));
    if (!expected_value.has_value()) {
        return std::nullopt;
    }

    return std::make_pair(condition.substr(0, delimiter), *expected_value);
}

bool WorkflowBooleanConditionsSatisfied(const WorkflowDefinition& workflow, const TaskRequest& task) {
    return std::all_of(
        workflow.input_bool.begin(),
        workflow.input_bool.end(),
        [&](const std::string& condition) {
            const auto parsed_condition = ParseBooleanCondition(condition);
            if (!parsed_condition.has_value()) {
                return false;
            }

            const auto input = task.inputs.find(parsed_condition->first);
            if (input == task.inputs.end()) {
                return false;
            }

            const auto input_value = ParseBooleanValue(input->second);
            return input_value.has_value() && *input_value == parsed_condition->second;
        });
}

bool WorkflowRegexConditionsSatisfied(const WorkflowDefinition& workflow, const TaskRequest& task) {
    return std::all_of(
        workflow.input_regex.begin(),
        workflow.input_regex.end(),
        [&](const std::string& condition) {
            const auto delimiter = condition.find('=');
            if (delimiter == std::string::npos || delimiter == 0) {
                return false;
            }

            const auto input_name = condition.substr(0, delimiter);
            const auto pattern = condition.substr(delimiter + 1);
            const auto input = task.inputs.find(input_name);
            if (input == task.inputs.end()) {
                return false;
            }

            try {
                return std::regex_match(input->second, std::regex(pattern));
            } catch (const std::regex_error&) {
                return false;
            }
        });
}

bool WorkflowCompositeConditionSatisfied(const std::string& condition, const TaskRequest& task) {
    const auto type_delimiter = condition.find(':');
    if (type_delimiter == std::string::npos || type_delimiter == 0) {
        return false;
    }

    const auto condition_type = condition.substr(0, type_delimiter);
    const auto condition_value = condition.substr(type_delimiter + 1);
    if (condition_type == "exists") {
        return !condition_value.empty() && task.inputs.contains(condition_value);
    }
    if (condition_type == "equals") {
        WorkflowDefinition workflow;
        workflow.input_equals = {condition_value};
        return WorkflowInputEqualsSatisfied(workflow, task);
    }
    if (condition_type == "number_gte") {
        WorkflowDefinition workflow;
        workflow.input_number_gte = {condition_value};
        return WorkflowNumericConditionsSatisfied(workflow, task);
    }
    if (condition_type == "number_lte") {
        WorkflowDefinition workflow;
        workflow.input_number_lte = {condition_value};
        return WorkflowNumericConditionsSatisfied(workflow, task);
    }
    if (condition_type == "bool") {
        WorkflowDefinition workflow;
        workflow.input_bool = {condition_value};
        return WorkflowBooleanConditionsSatisfied(workflow, task);
    }
    if (condition_type == "regex") {
        WorkflowDefinition workflow;
        workflow.input_regex = {condition_value};
        return WorkflowRegexConditionsSatisfied(workflow, task);
    }
    return false;
}

bool WorkflowInputAnySatisfied(const WorkflowDefinition& workflow, const TaskRequest& task) {
    return std::all_of(
        workflow.input_any.begin(),
        workflow.input_any.end(),
        [&](const std::string& group) {
            std::size_t start = 0;
            while (start <= group.size()) {
                const auto delimiter = group.find('|', start);
                const auto condition = delimiter == std::string::npos
                    ? group.substr(start)
                    : group.substr(start, delimiter - start);
                if (!condition.empty() && WorkflowCompositeConditionSatisfied(condition, task)) {
                    return true;
                }
                if (delimiter == std::string::npos) {
                    break;
                }
                start = delimiter + 1;
            }
            return false;
        });
}

class WorkflowExpressionParser {
public:
    WorkflowExpressionParser(std::string expression, const TaskRequest& task)
        : expression_(std::move(expression)),
          task_(task) {}

    bool evaluate() {
        const auto value = parse_or();
        skip_spaces();
        return !failed_ && position_ == expression_.size() && value;
    }

private:
    bool parse_or() {
        auto value = parse_and();
        while (!failed_) {
            skip_spaces();
            if (!consume("||")) {
                break;
            }
            const auto right = parse_and();
            value = value || right;
        }
        return value;
    }

    bool parse_and() {
        auto value = parse_unary();
        while (!failed_) {
            skip_spaces();
            if (!consume("&&")) {
                break;
            }
            const auto right = parse_unary();
            value = value && right;
        }
        return value;
    }

    bool parse_unary() {
        skip_spaces();
        if (consume("!")) {
            return !parse_unary();
        }
        return parse_primary();
    }

    bool parse_primary() {
        skip_spaces();
        if (consume("(")) {
            const auto value = parse_or();
            skip_spaces();
            if (!consume(")")) {
                failed_ = true;
                return false;
            }
            return value;
        }

        const auto start = position_;
        while (position_ < expression_.size() &&
               !std::isspace(static_cast<unsigned char>(expression_[position_])) &&
               expression_[position_] != '(' &&
               expression_[position_] != ')' &&
               expression_[position_] != '!' &&
               expression_[position_] != '&' &&
               expression_[position_] != '|') {
            ++position_;
        }
        if (position_ == start) {
            failed_ = true;
            return false;
        }

        return WorkflowCompositeConditionSatisfied(expression_.substr(start, position_ - start), task_);
    }

    bool consume(const std::string_view token) {
        skip_spaces();
        if (expression_.compare(position_, token.size(), token) != 0) {
            return false;
        }
        position_ += token.size();
        return true;
    }

    void skip_spaces() {
        while (position_ < expression_.size() && std::isspace(static_cast<unsigned char>(expression_[position_]))) {
            ++position_;
        }
    }

    std::string expression_;
    const TaskRequest& task_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

bool WorkflowInputExpressionsSatisfied(const WorkflowDefinition& workflow, const TaskRequest& task) {
    return std::all_of(
        workflow.input_expr.begin(),
        workflow.input_expr.end(),
        [&](const std::string& expression) {
            return WorkflowExpressionParser(expression, task).evaluate();
        });
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
        if (!WorkflowInputEqualsSatisfied(workflow, task)) {
            continue;
        }
        if (!WorkflowNumericConditionsSatisfied(workflow, task)) {
            continue;
        }
        if (!WorkflowBooleanConditionsSatisfied(workflow, task)) {
            continue;
        }
        if (!WorkflowRegexConditionsSatisfied(workflow, task)) {
            continue;
        }
        if (!WorkflowInputAnySatisfied(workflow, task)) {
            continue;
        }
        if (!WorkflowInputExpressionsSatisfied(workflow, task)) {
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
