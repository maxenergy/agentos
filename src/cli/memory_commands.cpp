#include "cli/memory_commands.hpp"

#include "memory/memory_manager.hpp"
#include "memory/workflow_validation.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace agentos {

namespace {

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument.rfind("--", 0) == 0) {
            argument = argument.substr(2);
        }

        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        options[argument.substr(0, separator)] = argument.substr(separator + 1);
    }
    return options;
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

void PrintMemoryUsage() {
    std::cerr
        << "memory commands:\n"
        << "  agentos memory summary\n"
        << "  agentos memory stats\n"
        << "  agentos memory workflows\n"
        << "  agentos memory stored-workflows [enabled=true|false] [trigger_task_type=write_file] [source=promoted_candidate] [name_contains=write]\n"
        << "  agentos memory show-workflow <workflow_name>\n"
        << "  agentos memory update-workflow <workflow_name> [new_name=<stored_name>] [trigger_task_type=write_file] [steps=file_write,file_read] [enabled=true|false] [required_inputs=a,b] [input_equals=mode=fast]\n"
        << "  agentos memory clone-workflow <workflow_name> new_name=<stored_name>\n"
        << "  agentos memory set-workflow-enabled <workflow_name> enabled=true|false\n"
        << "  agentos memory remove-workflow <workflow_name>\n"
        << "  agentos memory validate-workflows\n"
        << "  agentos memory explain-workflow <workflow_name> [task_type=<task_type>] key=value ...\n"
        << "  agentos memory lessons\n"
        << "  agentos memory promote-workflow <candidate_name> [workflow=<stored_name>] [required_inputs=a,b] [input_equals=mode=fast,status=ready] [input_number_gte=priority=5] [input_number_lte=size=10] [input_bool=approved=true] [input_regex=path=src/.*] [input_any=equals:mode=fast|equals:mode=safe] [input_expr=\"equals:mode=fast&&(exists:ticket||regex:branch=release/.*)\"]\n";
}

void PrintMemorySummary(const MemoryManager& memory_manager) {
    std::cout
        << "tasks=" << memory_manager.task_log().size()
        << " skills=" << memory_manager.skill_stats().size()
        << " agents=" << memory_manager.agent_stats().size()
        << " workflow_candidates=" << memory_manager.workflow_candidates().size()
        << " workflows=" << memory_manager.workflow_store().list().size()
        << " lessons=" << memory_manager.lesson_store().list().size()
        << '\n';
}

void PrintMemoryStats(const MemoryManager& memory_manager) {
    for (const auto& [name, stats] : memory_manager.skill_stats()) {
        const auto success_rate = stats.total_calls == 0
            ? 0.0
            : static_cast<double>(stats.success_calls) / static_cast<double>(stats.total_calls);
        std::cout
            << "skill " << name
            << " total=" << stats.total_calls
            << " success=" << stats.success_calls
            << " success_rate=" << success_rate
            << " avg_latency_ms=" << stats.avg_latency_ms
            << '\n';
    }

    for (const auto& [name, stats] : memory_manager.agent_stats()) {
        const auto success_rate = stats.total_runs == 0
            ? 0.0
            : static_cast<double>(stats.success_runs) / static_cast<double>(stats.total_runs);
        std::cout
            << "agent " << name
            << " total=" << stats.total_runs
            << " success=" << stats.success_runs
            << " failed=" << stats.failed_runs
            << " success_rate=" << success_rate
            << " avg_duration_ms=" << stats.avg_duration_ms
            << '\n';
    }
}

void PrintWorkflowCandidates(const MemoryManager& memory_manager) {
    for (const auto& workflow : memory_manager.workflow_candidates()) {
        std::cout
            << workflow.name
            << " trigger=" << workflow.trigger_task_type
            << " score=" << workflow.score
            << " use_count=" << workflow.use_count
            << " success_count=" << workflow.success_count
            << " failure_count=" << workflow.failure_count
            << " success_rate=" << workflow.success_rate
            << " avg_duration_ms=" << workflow.avg_duration_ms
            << " steps=";

        for (std::size_t index = 0; index < workflow.ordered_steps.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << workflow.ordered_steps[index];
        }
        std::cout << '\n';
    }
}

void PrintWorkflowDefinition(const WorkflowDefinition& workflow) {
    std::cout
        << workflow.name
        << " enabled=" << (workflow.enabled ? "true" : "false")
        << " trigger=" << workflow.trigger_task_type
        << " source=" << workflow.source
        << " score=" << workflow.score
        << " use_count=" << workflow.use_count
        << " success_count=" << workflow.success_count
        << " failure_count=" << workflow.failure_count
        << " success_rate=" << workflow.success_rate
        << " avg_duration_ms=" << workflow.avg_duration_ms
        << " required_inputs=";

    for (std::size_t index = 0; index < workflow.required_inputs.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.required_inputs[index];
    }

    std::cout << " input_equals=";
    for (std::size_t index = 0; index < workflow.input_equals.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_equals[index];
    }

    std::cout << " input_number_gte=";
    for (std::size_t index = 0; index < workflow.input_number_gte.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_number_gte[index];
    }

    std::cout << " input_number_lte=";
    for (std::size_t index = 0; index < workflow.input_number_lte.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_number_lte[index];
    }

    std::cout << " input_bool=";
    for (std::size_t index = 0; index < workflow.input_bool.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_bool[index];
    }

    std::cout << " input_regex=";
    for (std::size_t index = 0; index < workflow.input_regex.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_regex[index];
    }

    std::cout << " input_any=";
    for (std::size_t index = 0; index < workflow.input_any.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_any[index];
    }

    std::cout << " input_expr=";
    for (std::size_t index = 0; index < workflow.input_expr.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.input_expr[index];
    }

    std::cout
        << " steps=";

    for (std::size_t index = 0; index < workflow.ordered_steps.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.ordered_steps[index];
    }
    std::cout << '\n';
}

bool WorkflowDefinitionMatchesFilters(
    const WorkflowDefinition& workflow,
    const std::map<std::string, std::string>& filters) {
    if (filters.contains("enabled")) {
        const auto enabled = filters.at("enabled");
        if (enabled != "true" && enabled != "false") {
            return false;
        }
        if (workflow.enabled != (enabled == "true")) {
            return false;
        }
    }
    if (filters.contains("trigger_task_type") && workflow.trigger_task_type != filters.at("trigger_task_type")) {
        return false;
    }
    if (filters.contains("trigger") && workflow.trigger_task_type != filters.at("trigger")) {
        return false;
    }
    if (filters.contains("source") && workflow.source != filters.at("source")) {
        return false;
    }
    if (filters.contains("name_contains") &&
        workflow.name.find(filters.at("name_contains")) == std::string::npos) {
        return false;
    }
    return true;
}

void PrintWorkflowDefinitions(
    const WorkflowStore& workflow_store,
    const std::map<std::string, std::string>& filters = {}) {
    for (const auto& workflow : workflow_store.list()) {
        if (!WorkflowDefinitionMatchesFilters(workflow, filters)) {
            continue;
        }
        PrintWorkflowDefinition(workflow);
    }
}

void PrintWorkflowValidationDiagnostics(const std::vector<WorkflowValidationDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cout
            << "workflow_validation"
            << " workflow=" << diagnostic.workflow_name
            << " field=" << diagnostic.field
            << " value=" << diagnostic.value
            << " reason=" << diagnostic.reason
            << '\n';
    }
}

void PrintWorkflowApplicabilityEvaluation(const WorkflowApplicabilityEvaluation& evaluation) {
    std::cout
        << "workflow_applicability"
        << " workflow=" << evaluation.workflow_name
        << " applicable=" << (evaluation.applicable ? "true" : "false")
        << '\n';
    for (const auto& check : evaluation.checks) {
        std::cout
            << "workflow_applicability_check"
            << " field=" << check.field
            << " value=" << check.value
            << " matched=" << (check.matched ? "true" : "false")
            << " reason=" << check.reason
            << '\n';
    }
}

void PrintLessons(const LessonStore& lesson_store) {
    for (const auto& lesson : lesson_store.list()) {
        std::cout
            << lesson.lesson_id
            << " enabled=" << (lesson.enabled ? "true" : "false")
            << " task_type=" << lesson.task_type
            << " target=" << lesson.target_name
            << " error_code=" << lesson.error_code
            << " occurrences=" << lesson.occurrence_count
            << " last_task_id=" << lesson.last_task_id
            << " summary=\"" << lesson.summary << "\""
            << '\n';
    }
}

}  // namespace

int RunMemoryCommand(MemoryManager& memory_manager, const int argc, char* argv[]) {
    if (argc < 3) {
        PrintMemoryUsage();
        return 1;
    }

    const std::string command = argv[2];
    memory_manager.refresh_workflow_store();

    if (command == "summary") {
        PrintMemorySummary(memory_manager);
        return 0;
    }

    if (command == "stats") {
        PrintMemoryStats(memory_manager);
        return 0;
    }

    if (command == "workflows") {
        PrintWorkflowCandidates(memory_manager);
        return 0;
    }

    if (command == "stored-workflows") {
        const auto filters = ParseOptionsFromArgs(argc, argv, 3);
        if (filters.contains("enabled") && filters.at("enabled") != "true" && filters.at("enabled") != "false") {
            std::cerr << "enabled must be true or false\n";
            return 1;
        }
        PrintWorkflowDefinitions(memory_manager.workflow_store(), filters);
        return 0;
    }

    if (command == "show-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto workflow_name = options.contains("workflow")
            ? options.at("workflow")
            : (options.contains("name")
                ? options.at("name")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (workflow_name.empty()) {
            std::cerr << "workflow name is required\n";
            return 1;
        }

        const auto workflow = memory_manager.workflow_store().find(workflow_name);
        if (!workflow.has_value()) {
            std::cerr << "workflow not found: " << workflow_name << '\n';
            return 1;
        }

        PrintWorkflowDefinition(*workflow);
        return 0;
    }

    if (command == "set-workflow-enabled") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto workflow_name = options.contains("workflow")
            ? options.at("workflow")
            : (options.contains("name")
                ? options.at("name")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (workflow_name.empty()) {
            std::cerr << "workflow name is required\n";
            return 1;
        }
        if (!options.contains("enabled") ||
            (options.at("enabled") != "true" && options.at("enabled") != "false")) {
            std::cerr << "enabled must be true or false\n";
            return 1;
        }

        auto workflow = memory_manager.workflow_store().find(workflow_name);
        if (!workflow.has_value()) {
            std::cerr << "workflow not found: " << workflow_name << '\n';
            return 1;
        }

        workflow->enabled = options.at("enabled") == "true";
        PrintWorkflowDefinition(memory_manager.workflow_store().save(std::move(*workflow)));
        return 0;
    }

    if (command == "remove-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto workflow_name = options.contains("workflow")
            ? options.at("workflow")
            : (options.contains("name")
                ? options.at("name")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (workflow_name.empty()) {
            std::cerr << "workflow name is required\n";
            return 1;
        }

        const auto removed = memory_manager.workflow_store().remove(workflow_name);
        std::cout << (removed ? "removed " : "not_found ") << workflow_name << '\n';
        return removed ? 0 : 1;
    }

    if (command == "update-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto workflow_name = options.contains("workflow")
            ? options.at("workflow")
            : (options.contains("name")
                ? options.at("name")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (workflow_name.empty()) {
            std::cerr << "workflow name is required\n";
            return 1;
        }

        auto workflow = memory_manager.workflow_store().find(workflow_name);
        if (!workflow.has_value()) {
            std::cerr << "workflow not found: " << workflow_name << '\n';
            return 1;
        }

        if (options.contains("trigger_task_type")) {
            workflow->trigger_task_type = options.at("trigger_task_type");
        }
        if (options.contains("new_name")) {
            const auto new_name = options.at("new_name");
            if (new_name.empty()) {
                std::cerr << "new_name cannot be empty\n";
                return 1;
            }
            if (new_name != workflow_name && memory_manager.workflow_store().find(new_name).has_value()) {
                std::cerr << "workflow already exists: " << new_name << '\n';
                return 1;
            }
            workflow->name = new_name;
        }
        if (options.contains("steps")) {
            workflow->ordered_steps = SplitCommaList(options.at("steps"));
        }
        if (options.contains("enabled")) {
            if (options.at("enabled") != "true" && options.at("enabled") != "false") {
                std::cerr << "enabled must be true or false\n";
                return 1;
            }
            workflow->enabled = options.at("enabled") == "true";
        }
        if (options.contains("required_inputs")) {
            workflow->required_inputs = SplitCommaList(options.at("required_inputs"));
        }
        if (options.contains("input_equals")) {
            workflow->input_equals = SplitCommaList(options.at("input_equals"));
        }
        if (options.contains("input_number_gte")) {
            workflow->input_number_gte = SplitCommaList(options.at("input_number_gte"));
        }
        if (options.contains("input_number_lte")) {
            workflow->input_number_lte = SplitCommaList(options.at("input_number_lte"));
        }
        if (options.contains("input_bool")) {
            workflow->input_bool = SplitCommaList(options.at("input_bool"));
        }
        if (options.contains("input_regex")) {
            workflow->input_regex = SplitCommaList(options.at("input_regex"));
        }
        if (options.contains("input_any")) {
            workflow->input_any = SplitCommaList(options.at("input_any"));
        }
        if (options.contains("input_expr")) {
            workflow->input_expr = SplitCommaList(options.at("input_expr"));
        }

        const auto diagnostics = ValidateWorkflowDefinition(*workflow);
        if (!diagnostics.empty()) {
            PrintWorkflowValidationDiagnostics(diagnostics);
            return 1;
        }

        if (workflow->name != workflow_name) {
            (void)memory_manager.workflow_store().remove(workflow_name);
        }
        PrintWorkflowDefinition(memory_manager.workflow_store().save(std::move(*workflow)));
        return 0;
    }

    if (command == "clone-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto workflow_name = options.contains("workflow")
            ? options.at("workflow")
            : (options.contains("name")
                ? options.at("name")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        const auto new_name = options.contains("new_name")
            ? options.at("new_name")
            : (options.contains("target") ? options.at("target") : "");
        if (workflow_name.empty()) {
            std::cerr << "workflow name is required\n";
            return 1;
        }
        if (new_name.empty()) {
            std::cerr << "new_name is required\n";
            return 1;
        }
        if (memory_manager.workflow_store().find(new_name).has_value()) {
            std::cerr << "workflow already exists: " << new_name << '\n';
            return 1;
        }

        auto workflow = memory_manager.workflow_store().find(workflow_name);
        if (!workflow.has_value()) {
            std::cerr << "workflow not found: " << workflow_name << '\n';
            return 1;
        }

        workflow->name = new_name;
        workflow->source = "cloned_workflow";
        const auto diagnostics = ValidateWorkflowDefinition(*workflow);
        if (!diagnostics.empty()) {
            PrintWorkflowValidationDiagnostics(diagnostics);
            return 1;
        }

        PrintWorkflowDefinition(memory_manager.workflow_store().save(std::move(*workflow)));
        return 0;
    }

    if (command == "validate-workflows") {
        std::vector<WorkflowValidationDiagnostic> diagnostics;
        for (const auto& workflow : memory_manager.workflow_store().list()) {
            const auto workflow_diagnostics = ValidateWorkflowDefinition(workflow);
            diagnostics.insert(diagnostics.end(), workflow_diagnostics.begin(), workflow_diagnostics.end());
        }
        PrintWorkflowValidationDiagnostics(diagnostics);
        if (diagnostics.empty()) {
            std::cout << "workflow_validation valid=true\n";
        }
        return diagnostics.empty() ? 0 : 1;
    }

    if (command == "explain-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto workflow_name = options.contains("workflow")
            ? options.at("workflow")
            : (options.contains("name")
                ? options.at("name")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (workflow_name.empty()) {
            std::cerr << "workflow name is required\n";
            return 1;
        }

        const auto workflow = memory_manager.workflow_store().find(workflow_name);
        if (!workflow.has_value()) {
            std::cerr << "workflow not found: " << workflow_name << '\n';
            return 1;
        }

        const auto task_type = options.contains("task_type") ? options.at("task_type") : workflow->trigger_task_type;
        StringMap inputs;
        for (const auto& [key, value] : options) {
            if (key == "workflow" || key == "name" || key == "task_type") {
                continue;
            }
            inputs[key] = value;
        }

        PrintWorkflowApplicabilityEvaluation(EvaluateWorkflowApplicability(*workflow, task_type, inputs));
        return 0;
    }

    if (command == "lessons") {
        PrintLessons(memory_manager.lesson_store());
        return 0;
    }

    if (command == "promote-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto candidate_name = options.contains("name")
            ? options.at("name")
            : (options.contains("candidate")
                ? options.at("candidate")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (candidate_name.empty()) {
            std::cerr << "workflow candidate name is required\n";
            return 1;
        }

        const auto candidates = memory_manager.workflow_candidates();
        const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const WorkflowCandidate& candidate) {
            return candidate.name == candidate_name;
        });
        if (it == candidates.end()) {
            std::cerr << "workflow candidate not found: " << candidate_name << '\n';
            return 1;
        }

        auto workflow = WorkflowStore::FromCandidate(*it);
        workflow.source = "promoted_candidate";
        workflow.enabled = !options.contains("enabled") || options.at("enabled") != "false";
        if (options.contains("required_inputs")) {
            workflow.required_inputs = SplitCommaList(options.at("required_inputs"));
        }
        if (options.contains("input_equals")) {
            workflow.input_equals = SplitCommaList(options.at("input_equals"));
        }
        if (options.contains("input_number_gte")) {
            workflow.input_number_gte = SplitCommaList(options.at("input_number_gte"));
        }
        if (options.contains("input_number_lte")) {
            workflow.input_number_lte = SplitCommaList(options.at("input_number_lte"));
        }
        if (options.contains("input_bool")) {
            workflow.input_bool = SplitCommaList(options.at("input_bool"));
        }
        if (options.contains("input_regex")) {
            workflow.input_regex = SplitCommaList(options.at("input_regex"));
        }
        if (options.contains("input_any")) {
            workflow.input_any = SplitCommaList(options.at("input_any"));
        }
        if (options.contains("input_expr")) {
            workflow.input_expr = SplitCommaList(options.at("input_expr"));
        }
        if (options.contains("workflow")) {
            workflow.name = options.at("workflow");
        } else if (options.contains("workflow_name")) {
            workflow.name = options.at("workflow_name");
        }

        const auto diagnostics = ValidateWorkflowDefinition(workflow);
        if (!diagnostics.empty()) {
            PrintWorkflowValidationDiagnostics(diagnostics);
            return 1;
        }

        PrintWorkflowDefinition(memory_manager.workflow_store().save(std::move(workflow)));
        return 0;
    }

    PrintMemoryUsage();
    return 1;
}

}  // namespace agentos
