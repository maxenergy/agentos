#include "cli/interactive_commands.hpp"

#include "utils/signal_cancellation.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace agentos {

namespace {

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string MakeTaskId(const std::string& prefix) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return prefix + "-" + std::to_string(value);
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

// Tokenize a line by whitespace, respecting double-quoted spans.
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_quotes = false;
    for (const char ch : line) {
        if (ch == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes && (ch == ' ' || ch == '\t')) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += ch;
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::map<std::string, std::string> ParseKvArgs(const std::vector<std::string>& tokens, const size_t start) {
    std::map<std::string, std::string> options;
    for (size_t i = start; i < tokens.size(); ++i) {
        const auto sep = tokens[i].find('=');
        if (sep == std::string::npos) {
            continue;
        }
        options[tokens[i].substr(0, sep)] = tokens[i].substr(sep + 1);
    }
    return options;
}

TaskRequest BuildTaskFromTokens(const std::vector<std::string>& tokens,
                                 const std::filesystem::path& workspace) {
    TaskRequest task{
        .task_id = MakeTaskId("interactive"),
        .task_type = tokens.size() >= 2 ? tokens[1] : "",
        .objective = std::string("Interactive task: ") + (tokens.size() >= 2 ? tokens[1] : ""),
        .workspace_path = workspace,
    };

    const auto options = ParseKvArgs(tokens, 2);
    for (const auto& [key, value] : options) {
        if (key == "objective") {
            task.objective = value;
        } else if (key == "target") {
            task.preferred_target = value;
        } else if (key == "idempotency_key") {
            task.idempotency_key = value;
        } else if (key == "user" || key == "user_id") {
            task.user_id = value;
        } else if (key == "allow_network") {
            task.allow_network = value == "true";
        } else if (key == "allow_high_risk") {
            task.allow_high_risk = value == "true";
        } else if (key == "approval_id") {
            task.approval_id = value;
        } else if (key == "permission_grants" || key == "grants") {
            task.permission_grants = SplitCommaList(value);
        } else if (key == "timeout_ms") {
            try { task.timeout_ms = std::stoi(value); } catch (...) {}
        } else {
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

// ── Banner ──────────────────────────────────────────────────────────────────

void EnableUtf8Console() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
}

void PrintBanner() {
    EnableUtf8Console();
    std::cout
        << "\n"
        << "  +==================================================+\n"
        << "  |           AgentOS Interactive Console            |\n"
        << "  |  Type 'help' for available commands, 'exit' to  |\n"
        << "  |  quit. Ctrl-C interrupts a running task.        |\n"
        << "  +==================================================+\n"
        << "\n";
}

void PrintHelp() {
    std::cout
        << "Available commands:\n"
        << "  run <task_type> [key=value ...]   Execute a task through the agent loop\n"
        << "  agents                            List registered agent adapters\n"
        << "  skills                            List registered skills\n"
        << "  status                            Show runtime status summary\n"
        << "  memory summary                    Show memory summary\n"
        << "  memory stats                      Show skill/agent stats\n"
        << "  memory lessons                    Show lesson store\n"
        << "  memory workflows                  Show workflow candidates\n"
        << "  memory stored-workflows           Show stored workflows\n"
        << "  schedule list                     List scheduled tasks\n"
        << "  schedule history                  Show scheduler run history\n"
        << "  help                              Show this help message\n"
        << "  exit / quit                       Exit the interactive console\n"
        << "\n"
        << "Examples:\n"
        << "  run read_file path=README.md\n"
        << "  run write_file path=runtime/note.txt content=hello idempotency_key=demo\n"
        << "  run analysis target=local_planner objective=Plan_next_steps\n"
        << "\n";
}

}  // namespace

// ── Entry point ─────────────────────────────────────────────────────────────

int RunInteractiveCommand(
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    AgentLoop& loop,
    MemoryManager& memory_manager,
    Scheduler& scheduler,
    AuditLogger& audit_logger,
    const std::filesystem::path& workspace,
    const int /*argc*/,
    char* /*argv*/[]) {

    PrintBanner();

    auto cancel = InstallSignalCancellation();

    std::string line;
    while (true) {
        // Check if Ctrl-C was pressed between commands.
        if (cancel && cancel->is_cancelled()) {
            std::cout << "\nInterrupted. Exiting interactive console.\n";
            break;
        }

        std::cout << "agentos> " << std::flush;

        if (!std::getline(std::cin, line)) {
            // EOF (Ctrl-D on Unix, Ctrl-Z on Windows).
            std::cout << "\n";
            break;
        }

        // Trim whitespace.
        const auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            continue;  // empty line
        }
        line = line.substr(start);
        const auto end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            line = line.substr(0, end + 1);
        }

        const auto tokens = Tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        const auto& command = tokens[0];

        // ── exit / quit ─────────────────────────────────────────────────
        if (command == "exit" || command == "quit") {
            std::cout << "Goodbye.\n";
            break;
        }

        // ── help ────────────────────────────────────────────────────────
        if (command == "help") {
            PrintHelp();
            continue;
        }

        // ── run <task_type> key=value ... ───────────────────────────────
        if (command == "run") {
            if (tokens.size() < 2) {
                std::cerr << "Usage: run <task_type> [key=value ...]\n";
                continue;
            }

            const auto task = BuildTaskFromTokens(tokens, workspace);

            // Reset the cancellation token for each task so Ctrl-C during
            // a previous long run does not block new dispatches.
            auto task_cancel = InstallSignalCancellation();
            const auto result = loop.run(task, std::move(task_cancel));
            PrintResult(result);
            std::cout << "audit_log: " << audit_logger.log_path().string() << '\n';
            continue;
        }

        // ── agents ──────────────────────────────────────────────────────
        if (command == "agents") {
            const auto agents = agent_registry.list_profiles();
            if (agents.empty()) {
                std::cout << "No agents registered.\n";
            } else {
                std::cout << "Registered agents (" << agents.size() << "):\n";
                for (const auto& prof : agents) {
                    std::cout << "  " << prof.agent_name
                              << "  cost=" << prof.cost_tier
                              << "  streaming=" << (prof.supports_streaming ? "true" : "false")
                              << '\n';
                }
            }
            std::cout << '\n';
            continue;
        }

        // ── skills ──────────────────────────────────────────────────────
        if (command == "skills") {
            const auto skills = skill_registry.list();
            if (skills.empty()) {
                std::cout << "No skills registered.\n";
            } else {
                std::cout << "Registered skills (" << skills.size() << "):\n";
                for (const auto& m : skills) {
                    std::cout << "  " << m.name
                              << "  risk=" << m.risk_level
                              << "  idempotent=" << (m.idempotent ? "true" : "false")
                              << '\n';
                }
            }
            std::cout << '\n';
            continue;
        }

        // ── status ──────────────────────────────────────────────────────
        if (command == "status") {
            std::cout << "AgentOS Interactive Console Status\n";
            std::cout << "  workspace: " << workspace.string() << '\n';
            std::cout << "  skills: " << skill_registry.list().size() << '\n';
            std::cout << "  agents: " << agent_registry.list_profiles().size() << '\n';
            std::cout << "  scheduled_tasks: " << scheduler.list().size() << '\n';
            std::cout << "  workflow_candidates: " << memory_manager.workflow_candidates().size() << '\n';
            std::cout << "  audit_log: " << audit_logger.log_path().string() << '\n';
            std::cout << '\n';
            continue;
        }

        // ── memory subcommands ──────────────────────────────────────────
        if (command == "memory") {
            if (tokens.size() < 2) {
                std::cerr << "Usage: memory summary|stats|lessons|workflows|stored-workflows\n";
                continue;
            }
            const auto sub = tokens[1];
            if (sub == "summary") {
                std::cout << "task_log_entries: " << memory_manager.task_log().size() << '\n';
                std::cout << "workflow_candidates: " << memory_manager.workflow_candidates().size() << '\n';
                std::cout << '\n';
            } else if (sub == "stats") {
                const auto& skill_stats = memory_manager.skill_stats();
                const auto& agent_stats = memory_manager.agent_stats();
                std::cout << "Skill stats (" << skill_stats.size() << "):\n";
                for (const auto& [name, stats] : skill_stats) {
                    std::cout << "  " << name
                              << "  calls=" << stats.total_calls
                              << "  success=" << stats.success_calls
                              << "  avg_ms=" << stats.avg_latency_ms
                              << '\n';
                }
                std::cout << "Agent stats (" << agent_stats.size() << "):\n";
                for (const auto& [name, stats] : agent_stats) {
                    std::cout << "  " << name
                              << "  runs=" << stats.total_runs
                              << "  success=" << stats.success_runs
                              << "  avg_ms=" << stats.avg_duration_ms
                              << '\n';
                }
                std::cout << '\n';
            } else if (sub == "lessons") {
                const auto lessons = memory_manager.lesson_store().list();
                if (lessons.empty()) {
                    std::cout << "No lessons recorded.\n";
                } else {
                    std::cout << "Lessons (" << lessons.size() << "):\n";
                    for (const auto& lesson : lessons) {
                        std::cout << "  " << lesson.summary
                                  << "  count=" << lesson.occurrence_count
                                  << "  error=" << lesson.error_code
                                  << '\n';
                    }
                }
                std::cout << '\n';
            } else if (sub == "workflows") {
                const auto candidates = memory_manager.workflow_candidates();
                if (candidates.empty()) {
                    std::cout << "No workflow candidates.\n";
                } else {
                    std::cout << "Workflow candidates (" << candidates.size() << "):\n";
                    for (const auto& wf : candidates) {
                        std::cout << "  " << wf.name
                                  << "  trigger=" << wf.trigger_task_type
                                  << "  score=" << wf.score
                                  << "  use=" << wf.use_count
                                  << '\n';
                    }
                }
                std::cout << '\n';
            } else if (sub == "stored-workflows") {
                const auto stored = memory_manager.workflow_store().list();
                if (stored.empty()) {
                    std::cout << "No stored workflows.\n";
                } else {
                    std::cout << "Stored workflows (" << stored.size() << "):\n";
                    for (const auto& wf : stored) {
                        std::cout << "  " << wf.name
                                  << "  trigger=" << wf.trigger_task_type
                                  << "  enabled=" << (wf.enabled ? "true" : "false")
                                  << '\n';
                    }
                }
                std::cout << '\n';
            } else {
                std::cerr << "Unknown memory subcommand: " << sub << '\n';
            }
            continue;
        }

        // ── schedule subcommands ────────────────────────────────────────
        if (command == "schedule") {
            if (tokens.size() < 2) {
                std::cerr << "Usage: schedule list|history\n";
                continue;
            }
            const auto sub = tokens[1];
            if (sub == "list") {
                const auto tasks = scheduler.list();
                if (tasks.empty()) {
                    std::cout << "No scheduled tasks.\n";
                } else {
                    for (const auto& t : tasks) {
                        std::cout << "  " << t.schedule_id
                                  << "  enabled=" << (t.enabled ? "true" : "false")
                                  << "  task=" << t.task.task_type
                                  << "  runs=" << t.run_count
                                  << '\n';
                    }
                }
                std::cout << '\n';
            } else if (sub == "history") {
                const auto records = scheduler.run_history();
                if (records.empty()) {
                    std::cout << "No scheduler run history.\n";
                } else {
                    for (const auto& r : records) {
                        std::cout << "  " << r.schedule_id
                                  << "  task_id=" << r.task_id
                                  << "  success=" << (r.success ? "true" : "false")
                                  << '\n';
                    }
                }
                std::cout << '\n';
            } else {
                std::cerr << "Unknown schedule subcommand: " << sub << '\n';
            }
            continue;
        }

        // ── unknown command ─────────────────────────────────────────────
        std::cerr << "Unknown command: " << command << ". Type 'help' for available commands.\n";
    }

    return 0;
}

}  // namespace agentos
