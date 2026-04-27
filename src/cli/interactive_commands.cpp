#include "cli/interactive_commands.hpp"

#include "utils/signal_cancellation.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>
#include <cstdlib>
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
        } else if (key == "profile" || key == "auth_profile") {
            task.auth_profile = value;
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

void PrintBanner(const std::filesystem::path& workspace) {
    EnableUtf8Console();
    std::cout
        << "\n"
        << "  +==================================================+\n"
        << "  |           AgentOS Interactive Console            |\n"
        << "  |  Type 'help' for available commands, 'exit' to  |\n"
        << "  |  quit. Ctrl-C interrupts a running task.        |\n"
        << "  +==================================================+\n"
        << "  workspace: " << workspace.string() << "\n"
        << "  (override with AGENTOS_WORKSPACE if auth/state lives elsewhere)\n"
        << "\n";
}

void PrintHelp() {
    std::cout
        << "Available commands (slash prefix optional, e.g. /help):\n"
        << "  run <task_type> [key=value ...]   Execute a task through the agent loop\n"
        << "  chat <text>                       Send free-form text to a chat agent\n"
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
        << "Free-form text that doesn't match a command is sent as a chat\n"
        << "prompt to the first healthy chat agent. Override the target via\n"
        << "the AGENTOS_CHAT_TARGET env var (e.g. gemini, anthropic, openai,\n"
        << "qwen, codex_cli, local_planner). Use `agentos auth login <provider>`\n"
        << "before chatting if no agent is healthy yet.\n"
        << "\n"
        << "Examples:\n"
        << "  run read_file path=README.md\n"
        << "  run write_file path=runtime/note.txt content=hello idempotency_key=demo\n"
        << "  run analysis target=local_planner objective=Plan_next_steps\n"
        << "  run analysis target=qwen profile=work objective=Use_a_non_default_auth_profile\n"
        << "  chat What does this project do?\n"
        << "  你好                                (free-form, routed to default chat agent)\n"
        << "\n";
}

// Resolve a chat target. Honors AGENTOS_CHAT_TARGET first, then walks a
// preference list and returns the first healthy adapter's name. Returns
// empty if nothing is available — caller prints a helpful login hint.
std::string ResolveChatTarget(const AgentRegistry& agent_registry) {
    const auto try_target = [&](const std::string& name) -> std::string {
        const auto adapter = agent_registry.find(name);
        if (adapter && adapter->healthy()) {
            return name;
        }
        return {};
    };

    std::string env_target;
#ifdef _WIN32
    char* env_buf = nullptr;
    size_t env_len = 0;
    if (_dupenv_s(&env_buf, &env_len, "AGENTOS_CHAT_TARGET") == 0 && env_buf) {
        env_target.assign(env_buf);
        free(env_buf);
    }
#else
    if (const char* env = std::getenv("AGENTOS_CHAT_TARGET"); env != nullptr) {
        env_target.assign(env);
    }
#endif
    if (!env_target.empty()) {
        const std::string& requested = env_target;
        const auto adapter = agent_registry.find(requested);
        if (!adapter) {
            std::cerr << "AGENTOS_CHAT_TARGET=" << requested
                      << " is not a registered agent; falling back to auto-detect.\n";
        } else if (!adapter->healthy()) {
            std::cerr << "AGENTOS_CHAT_TARGET=" << requested
                      << " is registered but reports unhealthy; falling back to auto-detect.\n";
        } else {
            return requested;
        }
    }

    for (const auto& candidate : {"gemini", "anthropic", "openai", "qwen", "codex_cli", "local_planner"}) {
        if (const auto found = try_target(candidate); !found.empty()) {
            return found;
        }
    }
    return {};
}

void RunChatPrompt(const std::string& prompt,
                   AgentRegistry& agent_registry,
                   AgentLoop& loop,
                   AuditLogger& audit_logger,
                   const std::filesystem::path& workspace) {
    const auto target = ResolveChatTarget(agent_registry);
    if (target.empty()) {
        std::cerr
            << "No healthy chat agent found.\n"
            << "  - Run `agentos auth login gemini mode=browser_oauth` (or another provider) first.\n"
            << "  - Or set AGENTOS_CHAT_TARGET to a registered agent name (see `agents`).\n";
        return;
    }

    TaskRequest task{
        .task_id = MakeTaskId("interactive-chat"),
        .task_type = "chat",
        .objective = prompt,
        .workspace_path = workspace,
    };
    task.preferred_target = target;
    // Chat hits an external LLM CLI/REST round-trip, which routinely takes
    // 10–30s. The TaskRequest default of 5000ms makes "hi" time out before
    // the provider even responds, so chat dispatches use a 2-minute ceiling
    // unless the underlying agent has already been given a tighter task
    // timeout from somewhere upstream.
    if (task.timeout_ms <= 5000) {
        task.timeout_ms = 120000;
    }

    std::cout << "(routing to " << target << ")\n";
    auto task_cancel = InstallSignalCancellation();
    const auto result = loop.run(task, std::move(task_cancel));
    PrintResult(result);
    std::cout << "audit_log: " << audit_logger.log_path().string() << '\n';
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

    PrintBanner(workspace);

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

        // Allow slash-prefixed commands ("/help", "/exit", "/run ...") so
        // users coming from chat-style UIs feel at home. The slash is purely
        // cosmetic — strip it before dispatch.
        std::string command = tokens[0];
        if (!command.empty() && command.front() == '/') {
            command.erase(0, 1);
        }

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

        // ── chat <text> ─────────────────────────────────────────────────
        if (command == "chat") {
            if (tokens.size() < 2) {
                std::cerr << "Usage: chat <text...>\n";
                continue;
            }
            // Reuse the original line minus the "chat" prefix to preserve
            // multi-word prompts and any embedded `=` signs.
            const auto prompt_start = line.find_first_not_of(" \t",
                line[0] == '/' ? 5 : 4);  // "/chat" vs "chat"
            const std::string prompt = (prompt_start == std::string::npos)
                ? std::string{}
                : line.substr(prompt_start);
            RunChatPrompt(prompt, agent_registry, loop, audit_logger, workspace);
            continue;
        }

        // ── free-form fallback: route the entire line to a chat agent ───
        // This is what makes typing `你好` (or any non-command text) just
        // work, instead of bouncing off "Unknown command:". Slash-prefixed
        // tokens ("/something") that didn't match any known command are
        // still treated as unknown — slashes are reserved for commands.
        if (!tokens[0].empty() && tokens[0].front() == '/') {
            std::cerr << "Unknown command: " << tokens[0]
                      << ". Type 'help' for available commands.\n";
            continue;
        }
        RunChatPrompt(line, agent_registry, loop, audit_logger, workspace);
    }

    return 0;
}

}  // namespace agentos
