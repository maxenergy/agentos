#include "cli/subagents_commands.hpp"

#include "utils/signal_cancellation.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::string MakeTaskId(const std::string& prefix) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return prefix + "-" + std::to_string(value);
}

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

bool ParseBoolOption(const std::map<std::string, std::string>& options, const std::string& key, const bool fallback = false) {
    if (!options.contains(key)) {
        return fallback;
    }

    const auto value = options.at(key);
    return value == "true" || value == "1" || value == "yes";
}

int ParseIntOption(const std::map<std::string, std::string>& options, const std::string& key, const int fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stoi(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

double ParseDoubleOption(const std::map<std::string, std::string>& options, const std::string& key, const double fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stod(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

bool IsReservedSubagentOption(const std::string& key) {
    static const std::vector<std::string> reserved{
        "agents",
        "mode",
        "task",
        "task_type",
        "objective",
        "id",
        "task_id",
        "idempotency_key",
        "remote",
        "remote_trigger",
        "origin_identity",
        "origin_identity_id",
        "origin_device",
        "origin_device_id",
        "allow_network",
        "allow_high_risk",
        "approval_id",
        "permission_grants",
        "grants",
        "timeout_ms",
        "budget_limit",
    };

    return std::find(reserved.begin(), reserved.end(), key) != reserved.end();
}

TaskRequest BuildSubagentTaskFromOptions(
    const std::map<std::string, std::string>& options,
    const std::filesystem::path& workspace) {
    const auto task_type = options.contains("task_type")
        ? options.at("task_type")
        : (options.contains("task") ? options.at("task") : "analysis");

    TaskRequest task{
        .task_id = options.contains("task_id") ? options.at("task_id") : MakeTaskId("subagents"),
        .task_type = task_type,
        .objective = options.contains("objective") ? options.at("objective") : ("Coordinate subagents for: " + task_type),
        .workspace_path = workspace,
        .idempotency_key = options.contains("idempotency_key") ? options.at("idempotency_key") : "",
        .remote_trigger = ParseBoolOption(options, "remote", ParseBoolOption(options, "remote_trigger", false)),
        .origin_identity_id = options.contains("origin_identity")
            ? options.at("origin_identity")
            : (options.contains("origin_identity_id") ? options.at("origin_identity_id") : ""),
        .origin_device_id = options.contains("origin_device")
            ? options.at("origin_device")
            : (options.contains("origin_device_id") ? options.at("origin_device_id") : ""),
        .timeout_ms = ParseIntOption(options, "timeout_ms", 5000),
        .budget_limit = ParseDoubleOption(options, "budget_limit", 0.0),
        .allow_high_risk = ParseBoolOption(options, "allow_high_risk", false),
        .allow_network = ParseBoolOption(options, "allow_network", false),
        .approval_id = options.contains("approval_id") ? options.at("approval_id") : "",
        .permission_grants = options.contains("permission_grants")
            ? SplitCommaList(options.at("permission_grants"))
            : (options.contains("grants") ? SplitCommaList(options.at("grants")) : std::vector<std::string>{}),
    };

    for (const auto& [key, value] : options) {
        if (!IsReservedSubagentOption(key)) {
            task.inputs[key] = value;
        }
    }

    return task;
}

void PrintResult(const TaskRunResult& result) {
    std::cout << "success: " << (result.success ? "true" : "false") << '\n';
    std::cout << "from_cache: " << (result.from_cache ? "true" : "false") << '\n';
    std::cout << "route: " << route_target_kind_name(result.route_kind) << " -> " << result.route_target << '\n';
    std::cout << "summary: " << result.summary << '\n';

    if (!result.output_json.empty()) {
        std::cout << "output: " << result.output_json << '\n';
    }
    if (!result.error_code.empty()) {
        std::cout << "error_code: " << result.error_code << '\n';
    }
    if (!result.error_message.empty()) {
        std::cout << "error_message: " << result.error_message << '\n';
    }

    std::cout << '\n';
}

void PrintSubagentsUsage() {
    std::cerr
        << "subagents commands:\n"
        << "  agentos subagents run [agents=<agent[,agent]>] [mode=sequential|parallel] objective=text\n";
}

}  // namespace

int RunSubagentsCommand(
    SubagentManager& subagent_manager,
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintSubagentsUsage();
        return 1;
    }

    const auto command = std::string(argv[2]);
    const auto options = ParseOptionsFromArgs(argc, argv, 3);

    if (command == "run") {
        const auto agent_names = options.contains("agents")
            ? SplitCommaList(options.at("agents"))
            : std::vector<std::string>{};
        const auto mode = ParseSubagentExecutionMode(options.contains("mode") ? options.at("mode") : "sequential");
        const auto task = BuildSubagentTaskFromOptions(options, workspace);
        // Bind a process-wide Ctrl-C / SIGINT handler so the orchestrator can
        // unwind in-flight V2 invocations cooperatively. Second signal hard-
        // kills (handler restores OS default disposition and re-raises).
        auto cancel = InstallSignalCancellation();
        const auto result = subagent_manager.run(task, agent_names, mode, std::move(cancel));
        PrintResult(result);
        return result.success ? 0 : 1;
    }

    PrintSubagentsUsage();
    return 1;
}

}  // namespace agentos
