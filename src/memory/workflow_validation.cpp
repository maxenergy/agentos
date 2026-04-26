#include "memory/workflow_validation.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string_view>
#include <utility>

namespace agentos {

namespace {

std::optional<std::pair<std::string, std::string>> ParseAssignment(const std::string& value) {
    const auto delimiter = value.find('=');
    if (delimiter == std::string::npos || delimiter == 0) {
        return std::nullopt;
    }
    return std::make_pair(value.substr(0, delimiter), value.substr(delimiter + 1));
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsBooleanValue(const std::string& value) {
    const auto lower = ToLower(value);
    return lower == "true" || lower == "false" || lower == "1" || lower == "0";
}

bool IsNumberValue(const std::string& value) {
    try {
        std::size_t parsed = 0;
        const auto number = std::stod(value, &parsed);
        (void)number;
        return parsed == value.size();
    } catch (...) {
        return false;
    }
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

std::optional<double> ParseNumberValue(const std::string& value) {
    try {
        std::size_t parsed = 0;
        const auto number = std::stod(value, &parsed);
        if (parsed != value.size()) {
            return std::nullopt;
        }
        return number;
    } catch (...) {
        return std::nullopt;
    }
}

void AddDiagnostic(
    std::vector<WorkflowValidationDiagnostic>& diagnostics,
    const WorkflowDefinition& workflow,
    std::string field,
    std::string value,
    std::string reason) {
    diagnostics.push_back(WorkflowValidationDiagnostic{
        .workflow_name = workflow.name,
        .field = std::move(field),
        .value = std::move(value),
        .reason = std::move(reason),
    });
}

bool ValidateAtom(
    const std::string& atom,
    const WorkflowDefinition& workflow,
    const std::string& field,
    std::vector<WorkflowValidationDiagnostic>& diagnostics) {
    const auto type_delimiter = atom.find(':');
    if (type_delimiter == std::string::npos || type_delimiter == 0) {
        AddDiagnostic(diagnostics, workflow, field, atom, "condition atom must use type:value");
        return false;
    }

    const auto type = atom.substr(0, type_delimiter);
    const auto value = atom.substr(type_delimiter + 1);
    if (type == "exists") {
        if (value.empty()) {
            AddDiagnostic(diagnostics, workflow, field, atom, "exists condition requires an input name");
            return false;
        }
        return true;
    }
    if (type == "equals") {
        if (!ParseAssignment(value).has_value()) {
            AddDiagnostic(diagnostics, workflow, field, atom, "equals condition must use input=value");
            return false;
        }
        return true;
    }
    if (type == "number_gte" || type == "number_lte") {
        const auto parsed = ParseAssignment(value);
        if (!parsed.has_value() || !IsNumberValue(parsed->second)) {
            AddDiagnostic(diagnostics, workflow, field, atom, type + " condition must use input=number");
            return false;
        }
        return true;
    }
    if (type == "bool") {
        const auto parsed = ParseAssignment(value);
        if (!parsed.has_value() || !IsBooleanValue(parsed->second)) {
            AddDiagnostic(diagnostics, workflow, field, atom, "bool condition must use input=true|false|1|0");
            return false;
        }
        return true;
    }
    if (type == "regex") {
        const auto parsed = ParseAssignment(value);
        if (!parsed.has_value()) {
            AddDiagnostic(diagnostics, workflow, field, atom, "regex condition must use input=pattern");
            return false;
        }
        try {
            std::regex(parsed->second);
        } catch (const std::regex_error&) {
            AddDiagnostic(diagnostics, workflow, field, atom, "regex condition contains an invalid regular expression");
            return false;
        }
        return true;
    }

    AddDiagnostic(diagnostics, workflow, field, atom, "unknown condition atom type");
    return false;
}

bool AtomMatches(const std::string& atom, const StringMap& inputs) {
    const auto type_delimiter = atom.find(':');
    if (type_delimiter == std::string::npos || type_delimiter == 0) {
        return false;
    }

    const auto type = atom.substr(0, type_delimiter);
    const auto value = atom.substr(type_delimiter + 1);
    if (type == "exists") {
        return !value.empty() && inputs.contains(value);
    }

    const auto parsed = ParseAssignment(value);
    if (!parsed.has_value()) {
        return false;
    }
    const auto input = inputs.find(parsed->first);
    if (input == inputs.end()) {
        return false;
    }

    if (type == "equals") {
        return input->second == parsed->second;
    }
    if (type == "number_gte" || type == "number_lte") {
        const auto input_number = ParseNumberValue(input->second);
        const auto expected_number = ParseNumberValue(parsed->second);
        if (!input_number.has_value() || !expected_number.has_value()) {
            return false;
        }
        return type == "number_gte"
            ? *input_number >= *expected_number
            : *input_number <= *expected_number;
    }
    if (type == "bool") {
        const auto input_bool = ParseBooleanValue(input->second);
        const auto expected_bool = ParseBooleanValue(parsed->second);
        return input_bool.has_value() && expected_bool.has_value() && *input_bool == *expected_bool;
    }
    if (type == "regex") {
        try {
            return std::regex_match(input->second, std::regex(parsed->second));
        } catch (const std::regex_error&) {
            return false;
        }
    }
    return false;
}

class ExpressionEvaluator {
public:
    ExpressionEvaluator(std::string expression, const StringMap& inputs)
        : expression_(std::move(expression)),
          inputs_(inputs) {}

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
        return AtomMatches(expression_.substr(start, position_ - start), inputs_);
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
    const StringMap& inputs_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

class ExpressionValidator {
public:
    ExpressionValidator(
        std::string expression,
        const WorkflowDefinition& workflow,
        std::vector<WorkflowValidationDiagnostic>& diagnostics)
        : expression_(std::move(expression)),
          workflow_(workflow),
          diagnostics_(diagnostics) {}

    bool validate() {
        const auto parsed = parse_or();
        skip_spaces();
        if (!parsed || position_ != expression_.size()) {
            AddDiagnostic(diagnostics_, workflow_, "input_expr", expression_, "expression contains invalid syntax");
            return false;
        }
        return true;
    }

private:
    bool parse_or() {
        auto value = parse_and();
        while (value) {
            skip_spaces();
            if (!consume("||")) {
                break;
            }
            value = parse_and();
        }
        return value;
    }

    bool parse_and() {
        auto value = parse_unary();
        while (value) {
            skip_spaces();
            if (!consume("&&")) {
                break;
            }
            value = parse_unary();
        }
        return value;
    }

    bool parse_unary() {
        skip_spaces();
        if (consume("!")) {
            return parse_unary();
        }
        return parse_primary();
    }

    bool parse_primary() {
        skip_spaces();
        if (consume("(")) {
            const auto value = parse_or();
            skip_spaces();
            return value && consume(")");
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
            return false;
        }
        return ValidateAtom(expression_.substr(start, position_ - start), workflow_, "input_expr", diagnostics_);
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
    const WorkflowDefinition& workflow_;
    std::vector<WorkflowValidationDiagnostic>& diagnostics_;
    std::size_t position_ = 0;
};

}  // namespace

std::vector<WorkflowValidationDiagnostic> ValidateWorkflowDefinition(const WorkflowDefinition& workflow) {
    std::vector<WorkflowValidationDiagnostic> diagnostics;
    if (workflow.name.empty()) {
        AddDiagnostic(diagnostics, workflow, "name", "", "workflow name is required");
    }
    if (workflow.trigger_task_type.empty()) {
        AddDiagnostic(diagnostics, workflow, "trigger_task_type", "", "trigger task type is required");
    }
    if (workflow.ordered_steps.empty()) {
        AddDiagnostic(diagnostics, workflow, "ordered_steps", "", "at least one workflow step is required");
    }

    for (const auto& input : workflow.required_inputs) {
        if (input.empty()) {
            AddDiagnostic(diagnostics, workflow, "required_inputs", input, "required input name cannot be empty");
        }
    }
    for (const auto& condition : workflow.input_equals) {
        if (!ParseAssignment(condition).has_value()) {
            AddDiagnostic(diagnostics, workflow, "input_equals", condition, "condition must use input=value");
        }
    }
    for (const auto& condition : workflow.input_number_gte) {
        const auto parsed = ParseAssignment(condition);
        if (!parsed.has_value() || !IsNumberValue(parsed->second)) {
            AddDiagnostic(diagnostics, workflow, "input_number_gte", condition, "condition must use input=number");
        }
    }
    for (const auto& condition : workflow.input_number_lte) {
        const auto parsed = ParseAssignment(condition);
        if (!parsed.has_value() || !IsNumberValue(parsed->second)) {
            AddDiagnostic(diagnostics, workflow, "input_number_lte", condition, "condition must use input=number");
        }
    }
    for (const auto& condition : workflow.input_bool) {
        const auto parsed = ParseAssignment(condition);
        if (!parsed.has_value() || !IsBooleanValue(parsed->second)) {
            AddDiagnostic(diagnostics, workflow, "input_bool", condition, "condition must use input=true|false|1|0");
        }
    }
    for (const auto& condition : workflow.input_regex) {
        const auto parsed = ParseAssignment(condition);
        if (!parsed.has_value()) {
            AddDiagnostic(diagnostics, workflow, "input_regex", condition, "condition must use input=pattern");
            continue;
        }
        try {
            std::regex(parsed->second);
        } catch (const std::regex_error&) {
            AddDiagnostic(diagnostics, workflow, "input_regex", condition, "condition contains an invalid regular expression");
        }
    }
    for (const auto& group : workflow.input_any) {
        bool has_atom = false;
        std::size_t start = 0;
        while (start <= group.size()) {
            const auto delimiter = group.find('|', start);
            const auto atom = delimiter == std::string::npos ? group.substr(start) : group.substr(start, delimiter - start);
            if (!atom.empty()) {
                has_atom = true;
                ValidateAtom(atom, workflow, "input_any", diagnostics);
            }
            if (delimiter == std::string::npos) {
                break;
            }
            start = delimiter + 1;
        }
        if (!has_atom) {
            AddDiagnostic(diagnostics, workflow, "input_any", group, "input_any group must contain at least one condition atom");
        }
    }
    for (const auto& expression : workflow.input_expr) {
        if (expression.empty()) {
            AddDiagnostic(diagnostics, workflow, "input_expr", expression, "expression cannot be empty");
            continue;
        }
        ExpressionValidator(expression, workflow, diagnostics).validate();
    }

    return diagnostics;
}

WorkflowApplicabilityEvaluation EvaluateWorkflowApplicability(
    const WorkflowDefinition& workflow,
    const std::string& task_type,
    const StringMap& inputs) {
    WorkflowApplicabilityEvaluation evaluation{
        .workflow_name = workflow.name,
        .applicable = true,
    };

    auto add_check = [&](std::string field, std::string value, const bool matched, std::string reason) {
        evaluation.checks.push_back(WorkflowApplicabilityCheck{
            .field = std::move(field),
            .value = std::move(value),
            .matched = matched,
            .reason = std::move(reason),
        });
        evaluation.applicable = evaluation.applicable && matched;
    };

    add_check("enabled", workflow.enabled ? "true" : "false", workflow.enabled, "workflow must be enabled");
    add_check("trigger_task_type", workflow.trigger_task_type, workflow.trigger_task_type == task_type, "task_type must match");
    add_check("ordered_steps", std::to_string(workflow.ordered_steps.size()), !workflow.ordered_steps.empty(), "workflow must have at least one step");

    for (const auto& input_name : workflow.required_inputs) {
        add_check("required_inputs", input_name, inputs.contains(input_name), "required input must be present");
    }
    for (const auto& condition : workflow.input_equals) {
        add_check("input_equals", condition, AtomMatches("equals:" + condition, inputs), "input must equal expected value");
    }
    for (const auto& condition : workflow.input_number_gte) {
        add_check("input_number_gte", condition, AtomMatches("number_gte:" + condition, inputs), "input number must be >= expected value");
    }
    for (const auto& condition : workflow.input_number_lte) {
        add_check("input_number_lte", condition, AtomMatches("number_lte:" + condition, inputs), "input number must be <= expected value");
    }
    for (const auto& condition : workflow.input_bool) {
        add_check("input_bool", condition, AtomMatches("bool:" + condition, inputs), "input boolean must match expected value");
    }
    for (const auto& condition : workflow.input_regex) {
        add_check("input_regex", condition, AtomMatches("regex:" + condition, inputs), "input must match regex pattern");
    }
    for (const auto& group : workflow.input_any) {
        bool matched = false;
        std::size_t start = 0;
        while (start <= group.size()) {
            const auto delimiter = group.find('|', start);
            const auto atom = delimiter == std::string::npos ? group.substr(start) : group.substr(start, delimiter - start);
            matched = matched || (!atom.empty() && AtomMatches(atom, inputs));
            if (delimiter == std::string::npos) {
                break;
            }
            start = delimiter + 1;
        }
        add_check("input_any", group, matched, "at least one condition in the group must match");
    }
    for (const auto& expression : workflow.input_expr) {
        add_check("input_expr", expression, ExpressionEvaluator(expression, inputs).evaluate(), "expression must evaluate true");
    }

    return evaluation;
}

}  // namespace agentos
