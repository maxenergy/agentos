#include "cli/interactive_commands.hpp"

#include "core/execution/agent_event_runtime_store.hpp"
#include "cli/interactive_chat_state.hpp"
#include "cli/intent_classifier.hpp"
#include "cli/interactive_intent_registry.hpp"
#include "cli/main_route_action.hpp"
#include "storage/main_agent_store.hpp"
#include "utils/signal_cancellation.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cwchar>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

namespace agentos {

namespace {

// ── Helpers ─────────────────────────────────────────────────────────────────

constexpr int kInteractiveChatTimeoutMs = 120000;

std::string ShortenForConsole(const std::string& text, std::size_t max_chars = 120);
TaskRunResult ExecuteMainRouteAction(const MainRouteAction& action,
                                     SkillRegistry& skill_registry,
                                     AgentRegistry& agent_registry,
                                     const std::filesystem::path& workspace,
                                     AgentLoop& loop,
                                     AuditLogger& audit_logger);

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

std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator) {
    std::ostringstream out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << separator;
        }
        out << values[i];
    }
    return out.str();
}

std::string JoinAgentCapabilities(const std::vector<AgentCapability>& values) {
    std::vector<std::string> names;
    names.reserve(values.size());
    for (const auto& cap : values) {
        if (!cap.name.empty()) {
            names.push_back(cap.name);
        }
    }
    return JoinStrings(names, ",");
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

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string ShortenForConsole(const std::string& text, std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "...";
}

struct AgentSkillSummary {
    std::string name;
    std::filesystem::path path;
};

std::vector<AgentSkillSummary> ListRepoAgentSkills(const std::filesystem::path& workspace) {
    std::vector<AgentSkillSummary> skills;
    const auto root = workspace / ".agents" / "skills";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return skills;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto skill_file = entry.path() / "SKILL.md";
        if (!std::filesystem::exists(skill_file, ec) || !std::filesystem::is_regular_file(skill_file, ec)) {
            continue;
        }
        skills.push_back({entry.path().filename().string(), skill_file});
    }
    std::sort(skills.begin(), skills.end(), [](const AgentSkillSummary& lhs, const AgentSkillSummary& rhs) {
        return lhs.name < rhs.name;
    });
    return skills;
}

bool LooksLikeMemoryQuestion(const std::string& line) {
    static const std::regex memory_re(
        R"((\b(what\s+do\s+you\s+remember|what\s+is\s+in\s+memory|show\s+memory|memory\s+summary|remembered|lessons?)\b)|你.*(记得|记住|记忆|学到).*(什么|哪些|内容)|你(还)?记得什么|你记住了什么|你的记忆|记忆里有什么|学到了什么|记住了哪些|记得哪些|详细记忆|完整记忆|全部记忆|记住的教训|可复用工作流|记忆详情)",
        std::regex_constants::icase);
    return std::regex_search(line, memory_re);
}

bool LooksLikeDetailedMemoryQuestion(const std::string& line) {
    static const std::regex detail_re(
        R"((\b(full|raw|detailed|details|stats|debug)\s+memory\b)|详细.*(记忆|memory)|完整.*(记忆|memory)|全部.*(记忆|memory)|记忆.*(明细|详情|详细|完整)|memory\s+(details|stats|raw))",
        std::regex_constants::icase);
    return std::regex_search(line, detail_re);
}

bool LooksLikeModelIdentityQuestion(const std::string& line) {
    return LooksLikeModelIdentityIntent(line);
}

bool TryConfigureMainAgentFromNaturalLanguage(const std::string& line,
                                              const std::filesystem::path& workspace) {
    if (!LooksLikeMainAgentConfigIntent(line)) {
        return false;
    }

    const auto ollama_model = ExtractOllamaModelName(line);
    if (!ollama_model.has_value() || ollama_model->empty()) {
        std::cout << "这是本地 main-agent 配置任务，不需要 Codex。\n"
                  << "我还不能从这句话里可靠解析模型名。可以直接使用：\n"
                  << "  main-agent set provider=openai-chat base_url=http://127.0.0.1:11434/v1 model=<ollama-model> api_key=EMPTY\n\n";
        return true;
    }

    MainAgentConfig config;
    config.provider_kind = "openai-chat";
    config.base_url = "http://127.0.0.1:11434/v1";
    config.model = *ollama_model;
    config.api_key = "EMPTY";

    const MainAgentStore store(workspace / "runtime" / "main_agent.tsv");
    if (!store.save(config)) {
        std::cerr << "main-agent: failed to write " << store.path().string() << "\n\n";
        return true;
    }

    std::cout << "main-agent: saved\n"
              << "provider_kind: " << config.provider_kind << '\n'
              << "base_url:      " << config.base_url << '\n'
              << "model:         " << config.model << '\n'
              << "api_key:       (set, literal placeholder)\n"
              << "config_path:   " << store.path().string() << "\n\n";
    return true;
}

bool LooksLikeSkillListQuestion(const std::string& line) {
    static const std::regex skills_re(
        R"((\b(what\s+(skills?|abilities|capabilities)\s+(do\s+you\s+have|are\s+available)|list\s+skills?|show\s+skills?|available\s+skills?|what\s+can\s+you\s+do)\b)|你.*(有什么|有哪些|会什么|能做什么).*(技能|能力)|有哪些.*(技能|能力)|列出.*(技能|能力)|技能.*(有哪些|列表|清单))",
        std::regex_constants::icase);
    return std::regex_search(line, skills_re);
}

bool LooksLikeAgentListQuestion(const std::string& line) {
    static const std::regex agents_re(
        R"((\b(what\s+agents?\s+(do\s+you\s+have|are\s+available)|list\s+agents?|show\s+agents?|available\s+agents?|registered\s+agents?)\b)|(登记|注册|可用|有哪些|列出).*(agent|agents|代理|智能体)|(agent|agents|代理|智能体).*(有哪些|列表|清单|如何用|怎么用))",
        std::regex_constants::icase);
    return std::regex_search(line, agents_re);
}

bool LooksLikeSpecificSkillUsageQuestion(const std::string& line) {
    static const std::regex usage_re(
        R"((\b(how\s+to\s+use|usage|help|args?|arguments?|examples?)\b)|如何使用|怎么使用|如何用|怎么用|用法|参数|示例|例子|能做什么|有什么用)",
        std::regex_constants::icase);
    return std::regex_search(line, usage_re);
}

bool LooksLikeBrowserConnectionError(const std::string& line) {
    static const std::regex error_re(
        R"((ERR_CONNECTION_REFUSED|connection\s+refused|refused\s+to\s+connect|site\s+can'?t\s+be\s+reached|127\.0\.0\.1.*refused|localhost.*refused|checking\s+the\s+connection|checking\s+the\s+proxy\s+and\s+the\s+firewall|代理|防火墙|无法访问|拒绝连接))",
        std::regex_constants::icase);
    return std::regex_search(line, error_re);
}

std::vector<std::pair<std::string, SkillStats>> TopSkillsByCalls(
    const std::unordered_map<std::string, SkillStats>& values,
    const std::size_t limit) {
    std::vector<std::pair<std::string, SkillStats>> entries;
    entries.reserve(values.size());
    for (const auto& [name, stats] : values) {
        entries.emplace_back(name, stats);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second.total_calls > rhs.second.total_calls;
    });
    if (entries.size() > limit) {
        entries.resize(limit);
    }
    return entries;
}

std::vector<std::pair<std::string, AgentRuntimeStats>> TopAgentsByRuns(
    const std::unordered_map<std::string, AgentRuntimeStats>& values,
    const std::size_t limit) {
    std::vector<std::pair<std::string, AgentRuntimeStats>> entries(values.begin(), values.end());
    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second.total_runs > rhs.second.total_runs;
    });
    if (entries.size() > limit) {
        entries.resize(limit);
    }
    return entries;
}

bool IsUserFacingMemoryObjective(const TaskMemoryRecord& task) {
    if (task.objective.empty() || task.task_type == "chat") {
        return false;
    }
    static const std::array<std::string, 5> internal_markers = {
        "Previous secondary-agent attempt failed primary acceptance",
        "Execute task type:",
        "Repair the work",
        "Write the deliverables manifest",
        "prompt"
    };
    for (const auto& marker : internal_markers) {
        if (task.objective.find(marker) != std::string::npos) {
            return false;
        }
    }
    return true;
}

void PrintMemoryOverview(const MemoryManager& memory_manager) {
    const auto& tasks = memory_manager.task_log();
    const auto lessons = memory_manager.lesson_store().list();
    const auto workflows = memory_manager.workflow_candidates();
    const auto stored_workflows = memory_manager.workflow_store().list();

    std::vector<std::string> recent_objectives;
    for (auto it = tasks.rbegin(); it != tasks.rend() && recent_objectives.size() < 3; ++it) {
        if (!IsUserFacingMemoryObjective(*it)) {
            continue;
        }
        const auto objective = ShortenForConsole(it->objective, 72);
        if (std::find(recent_objectives.begin(), recent_objectives.end(), objective) == recent_objectives.end()) {
            recent_objectives.push_back(objective);
        }
    }

    const auto top_skills = TopSkillsByCalls(memory_manager.skill_stats(), 4);
    const auto top_agents = TopAgentsByRuns(memory_manager.agent_stats(), 4);

    std::cout << "我记得这些重点：\n";
    if (!recent_objectives.empty()) {
        std::cout << "- 最近主要任务：";
        for (std::size_t i = 0; i < recent_objectives.size(); ++i) {
            if (i > 0) {
                std::cout << "；";
            }
            std::cout << recent_objectives[i];
        }
        std::cout << '\n';
    } else {
        std::cout << "- 最近还没有可总结的任务记录。\n";
    }

    std::cout << "- 已记录 " << lessons.size() << " 条经验教训、"
              << workflows.size() << " 个可推广 workflow 候选、"
              << stored_workflows.size() << " 个已保存 workflow。\n";

    if (!top_skills.empty()) {
        std::cout << "- 常用技能：";
        for (std::size_t i = 0; i < top_skills.size(); ++i) {
            if (i > 0) {
                std::cout << "、";
            }
            std::cout << top_skills[i].first;
        }
        std::cout << "。\n";
    }

    if (!top_agents.empty()) {
        std::cout << "- 常用代理：";
        for (std::size_t i = 0; i < top_agents.size(); ++i) {
            if (i > 0) {
                std::cout << "、";
            }
            std::cout << top_agents[i].first;
        }
        std::cout << "。\n";
    }

    if (!lessons.empty()) {
        std::cout << "- 最近学到的风险：";
        const auto count = std::min<std::size_t>(lessons.size(), 3);
        for (std::size_t i = 0; i < count; ++i) {
            if (i > 0) {
                std::cout << "；";
            }
            std::cout << ShortenForConsole(lessons[i].summary, 58);
        }
        std::cout << "。\n";
    }

    std::cout << "\n想看细节可以问：`详细记忆`，"
                 "或用命令 `memory stats|lessons|workflows|stored-workflows`。\n\n";
}

void PrintMemoryDetails(const MemoryManager& memory_manager) {
    const auto& tasks = memory_manager.task_log();
    const auto lessons = memory_manager.lesson_store().list();
    const auto workflows = memory_manager.workflow_candidates();
    const auto stored_workflows = memory_manager.workflow_store().list();

    std::cout << "AgentOS 本地记忆\n";
    if (!memory_manager.storage_dir().empty()) {
        std::cout << "  storage: " << memory_manager.storage_dir().string() << '\n';
    }
    std::cout << "  task_log_entries: " << tasks.size() << '\n'
              << "  lessons: " << lessons.size() << '\n'
              << "  workflow_candidates: " << workflows.size() << '\n'
              << "  stored_workflows: " << stored_workflows.size() << '\n';

    if (!tasks.empty()) {
        std::cout << "\n最近任务:\n";
        const auto start = tasks.size() > 5 ? tasks.size() - 5 : 0;
        for (std::size_t i = start; i < tasks.size(); ++i) {
            const auto& task = tasks[i];
            std::cout << "  " << task.task_id
                      << " type=" << task.task_type
                      << " success=" << (task.success ? "true" : "false")
                      << " duration_ms=" << task.duration_ms
                      << " objective=" << ShortenForConsole(task.objective) << '\n';
        }
    }

    const auto top_skills = TopSkillsByCalls(memory_manager.skill_stats(), 6);
    if (!top_skills.empty()) {
        std::cout << "\n常用 skill:\n";
        for (const auto& [name, stats] : top_skills) {
            std::cout << "  " << name
                      << " calls=" << stats.total_calls
                      << " success=" << stats.success_calls
                      << " avg_ms=" << stats.avg_latency_ms << '\n';
        }
    }

    const auto top_agents = TopAgentsByRuns(memory_manager.agent_stats(), 6);
    if (!top_agents.empty()) {
        std::cout << "\n常用 agent:\n";
        for (const auto& [name, stats] : top_agents) {
            std::cout << "  " << name
                      << " runs=" << stats.total_runs
                      << " success=" << stats.success_runs
                      << " failed=" << stats.failed_runs
                      << " avg_ms=" << stats.avg_duration_ms << '\n';
        }
    }

    if (!lessons.empty()) {
        std::cout << "\n记住的 lessons:\n";
        const auto count = std::min<std::size_t>(lessons.size(), 6);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& lesson = lessons[i];
            std::cout << "  " << ShortenForConsole(lesson.summary)
                      << " count=" << lesson.occurrence_count;
            if (!lesson.error_code.empty()) {
                std::cout << " error=" << lesson.error_code;
            }
            std::cout << '\n';
        }
    }

    if (!workflows.empty()) {
        std::cout << "\n可推广 workflow 候选:\n";
        const auto count = std::min<std::size_t>(workflows.size(), 6);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& workflow = workflows[i];
            std::cout << "  " << workflow.name
                      << " trigger=" << workflow.trigger_task_type
                      << " score=" << workflow.score
                      << " use=" << workflow.use_count << '\n';
        }
    }

    std::cout << "\n可用命令: memory summary | memory stats | memory lessons | memory workflows | memory stored-workflows\n\n";
}

std::optional<SkillManifest> FindMentionedSkill(const std::string& line,
                                                const SkillRegistry& skill_registry) {
    auto manifests = skill_registry.list();
    std::sort(manifests.begin(), manifests.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name.size() > rhs.name.size();
    });
    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    const auto lower_line = lower(line);
    for (const auto& manifest : manifests) {
        if (!manifest.name.empty() && lower_line.find(lower(manifest.name)) != std::string::npos) {
            return manifest;
        }
    }
    return std::nullopt;
}

void PrintMainModelIdentity(const AgentRegistry& agent_registry,
                            const std::filesystem::path& workspace) {
    const MainAgentStore store(workspace / "runtime" / "main_agent.tsv");
    const auto config = store.load();
    const auto main = agent_registry.find("main");
    std::cout << "当前首选聊天代理是 `main`。\n";
    if (!config.has_value()) {
        std::cout << "main-agent 还没有配置。可用 `agentos main-agent set ...` 设置模型。\n\n";
        return;
    }
    std::cout << "- provider: " << config->provider_kind << '\n'
              << "- model: " << config->model << '\n'
              << "- base_url: " << (config->base_url.empty() ? "(default)" : config->base_url) << '\n'
              << "- auth: " << (!config->api_key_env.empty()
                                    ? ("env:" + config->api_key_env)
                                    : (!config->api_key.empty() ? "literal api_key is set" :
                                       (!config->oauth_file.empty() ? ("oauth_file:" + config->oauth_file) : "(unset)")))
              << '\n';
    if (main) {
        std::cout << "- status: " << (main->healthy() ? "healthy" : "unhealthy") << '\n';
    }
    std::cout << "\n说明：AgentOS 是本地运行时；上面的 model 是普通聊天默认调用的底层模型。\n\n";
}

std::optional<nlohmann::json> FindLatestCompletedDevelopmentStatus(const std::filesystem::path& workspace) {
    const auto agents_root = workspace / "runtime" / "agents";
    std::error_code ec;
    if (!std::filesystem::exists(agents_root, ec) || !std::filesystem::is_directory(agents_root, ec)) {
        return std::nullopt;
    }

    std::filesystem::path latest_status;
    std::filesystem::file_time_type latest_time{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(agents_root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || entry.path().filename() != "status.json") {
            continue;
        }
        const auto path_text = entry.path().string();
        if (path_text.find("dev-") == std::string::npos) {
            continue;
        }
        const auto write_time = entry.last_write_time(ec);
        if (latest_status.empty() || write_time > latest_time) {
            latest_status = entry.path();
            latest_time = write_time;
        }
    }
    if (latest_status.empty()) {
        return std::nullopt;
    }
    try {
        auto status = nlohmann::json::parse(ReadTextFile(latest_status));
        status["_status_path"] = latest_status.string();
        if (status.value("state", std::string{}) != "completed" ||
            !status.value("success", false)) {
            return std::nullopt;
        }
        return status;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string ExtractFirstFencedCodeBlock(const std::string& text) {
    const auto open = text.find("```");
    if (open == std::string::npos) {
        return {};
    }
    const auto first_newline = text.find('\n', open + 3);
    if (first_newline == std::string::npos) {
        return {};
    }
    const auto close = text.find("```", first_newline + 1);
    if (close == std::string::npos || close <= first_newline + 1) {
        return {};
    }
    return text.substr(first_newline + 1, close - first_newline - 1);
}

bool PrintLocalBrowserErrorFollowup(const std::string& line,
                                    const std::filesystem::path& workspace) {
    if (!LooksLikeBrowserConnectionError(line)) {
        return false;
    }
    const auto status = FindLatestCompletedDevelopmentStatus(workspace);
    if (!status.has_value()) {
        return false;
    }

    const auto summary = status->value("summary", std::string{});
    const auto commands = ExtractFirstFencedCodeBlock(summary);
    std::cout << "(route: local_context -> latest_development_result)\n";
    std::cout << "你这个报错是在接着上一个开发任务说。刚才的验证只是在验收时临时启动了服务，验收结束后进程已经停止，所以浏览器访问 127.0.0.1 会出现 ERR_CONNECTION_REFUSED。\n\n";
    if (!commands.empty()) {
        std::cout << "需要先在 PowerShell 里启动生成的服务器：\n\n"
                  << "```powershell\n" << commands << "```\n\n";
    }
    std::cout << "然后打开 `http://127.0.0.1:8080/`。如果你换了端口，例如 `-p 18080`，浏览器也要打开 `http://127.0.0.1:18080/`。\n";
    if (status->contains("_status_path") && (*status)["_status_path"].is_string()) {
        std::cout << "最近任务状态: " << (*status)["_status_path"].get<std::string>() << "\n";
    }
    std::cout << '\n';
    return true;
}

void PrintRegisteredSkillsGuide(const SkillRegistry& skill_registry,
                                const std::filesystem::path& workspace) {
    const auto skills = skill_registry.list();
    if (skills.empty()) {
        std::cout << "当前没有注册 skill。\n\n";
        return;
    }
    std::vector<SkillManifest> healthy_skills;
    std::vector<std::string> unavailable_skills;
    for (const auto& m : skills) {
        const auto adapter = skill_registry.find(m.name);
        if (adapter && adapter->healthy()) {
            healthy_skills.push_back(m);
        } else {
            unavailable_skills.push_back(m.name);
        }
    }

    if (healthy_skills.empty()) {
        std::cout << "当前没有可用 skill。\n\n";
    } else {
        std::cout << "可用 skill (" << healthy_skills.size() << "):\n";
    }
    for (const auto& m : healthy_skills) {
        std::cout << "  " << m.name
                  << "  状态=健康";
        if (!m.description.empty()) {
            std::cout << "  " << m.description;
        }
        if (!m.capabilities.empty()) {
            std::cout << " capabilities=" << JoinStrings(m.capabilities, ",");
        }
        if (!m.risk_level.empty()) {
            std::cout << " risk=" << m.risk_level;
        }
        std::cout << '\n';
    }
    if (!unavailable_skills.empty()) {
        std::cout << "\n未列出的不可用 skill (" << unavailable_skills.size()
                  << "): " << JoinStrings(unavailable_skills, ",") << '\n'
                  << "原因通常是当前系统缺少对应 CLI 依赖；它们不会作为可用技能展示。\n";
    }
    std::cout << "\n使用方式：\n"
              << "  run <skill_name> key=value ...\n"
              << "  skills\n"
              << "  如何使用 <skill_name> 技能？\n\n";

    const auto agent_skills = ListRepoAgentSkills(workspace);
    if (!agent_skills.empty()) {
        std::cout << "仓库级 agent skills (" << agent_skills.size()
                  << ", 供 Codex/Claude 等代理读取，不是 `run` 调用的 runtime skill):\n";
        for (const auto& skill : agent_skills) {
            std::cout << "  " << skill.name << "  path=" << skill.path.string() << '\n';
        }
        std::cout << '\n';
    }
}

void PrintRegisteredAgentsGuide(const AgentRegistry& agent_registry) {
    const auto agents = agent_registry.list_profiles();
    if (agents.empty()) {
        std::cout << "当前没有注册 agent。\n\n";
        return;
    }
    std::cout << "已登记的 agent (" << agents.size() << "):\n";
    for (const auto& prof : agents) {
        const auto adapter = agent_registry.find(prof.agent_name);
        std::cout << "  " << prof.agent_name
                  << "  状态=" << (adapter && adapter->healthy() ? "健康" : "异常");
        if (!prof.description.empty()) {
            std::cout << "  " << prof.description;
        }
        const auto caps = JoinAgentCapabilities(prof.capabilities);
        if (!caps.empty()) {
            std::cout << " capabilities=" << caps;
        }
        if (!prof.cost_tier.empty()) {
            std::cout << " cost=" << prof.cost_tier;
        }
        if (prof.supports_network) {
            std::cout << " network=true";
        }
        std::cout << '\n';
    }
    std::cout << "\n使用方式：\n"
              << "  普通聊天默认走 `main`。\n"
              << "  开发类任务会后台派发给开发 agent / skill。\n"
              << "  显式调度：run analysis target=<agent_name> objective=<text>\n\n";
}

void PrintSkillUsageGuide(const SkillManifest& manifest) {
    std::cout << "Skill `" << manifest.name << "` 用法\n";
    if (!manifest.description.empty()) {
        std::cout << "说明: " << manifest.description << '\n';
    }
    if (!manifest.capabilities.empty()) {
        std::cout << "能力: " << JoinStrings(manifest.capabilities, ",") << '\n';
    }
    if (!manifest.risk_level.empty()) {
        std::cout << "风险级别: " << manifest.risk_level << '\n';
    }
    if (!manifest.permissions.empty()) {
        std::cout << "权限: " << JoinStrings(manifest.permissions, ",") << '\n';
    }
    if (manifest.timeout_ms > 0) {
        std::cout << "超时: " << manifest.timeout_ms << "ms\n";
    }
    std::cout << "\n调用格式:\n"
              << "  run " << manifest.name << " key=value ...\n";
    if (!manifest.input_schema_json.empty()) {
        std::cout << "\n输入 schema:\n" << manifest.input_schema_json << '\n';
    }
    std::cout << '\n';
}

// ── Banner ──────────────────────────────────────────────────────────────────

class ConsoleCodePageGuard {
public:
    ConsoleCodePageGuard() {
#ifdef _WIN32
        DWORD mode = 0;
        const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        output_is_console_ = output != INVALID_HANDLE_VALUE && output != nullptr &&
                             GetConsoleMode(output, &mode) != 0;
        input_is_console_ = input != INVALID_HANDLE_VALUE && input != nullptr &&
                            GetConsoleMode(input, &mode) != 0;
        original_output_cp_ = GetConsoleOutputCP();
        original_input_cp_ = GetConsoleCP();
        if (output_is_console_) {
            SetConsoleOutputCP(65001);
        }
        if (input_is_console_) {
            SetConsoleCP(65001);
        }
#endif
    }

    ~ConsoleCodePageGuard() {
#ifdef _WIN32
        if (output_is_console_ && original_output_cp_ != 0) {
            SetConsoleOutputCP(original_output_cp_);
        }
        if (input_is_console_ && original_input_cp_ != 0) {
            SetConsoleCP(original_input_cp_);
        }
#endif
    }

    ConsoleCodePageGuard(const ConsoleCodePageGuard&) = delete;
    ConsoleCodePageGuard& operator=(const ConsoleCodePageGuard&) = delete;

private:
#ifdef _WIN32
    UINT original_output_cp_ = 0;
    UINT original_input_cp_ = 0;
    bool output_is_console_ = false;
    bool input_is_console_ = false;
#endif
};

#ifndef _WIN32
bool IsUtf8ContinuationByte(const unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

int Utf8ExpectedContinuationCount(const unsigned char byte) {
    if (byte >= 0xC2 && byte <= 0xDF) {
        return 1;
    }
    if (byte >= 0xE0 && byte <= 0xEF) {
        return 2;
    }
    if (byte >= 0xF0 && byte <= 0xF4) {
        return 3;
    }
    return 0;
}

std::size_t Utf8PreviousCodepointStart(const std::string& value, std::size_t cursor) {
    cursor = std::min(cursor, value.size());
    if (cursor == 0) {
        return 0;
    }
    --cursor;
    while (cursor > 0 && IsUtf8ContinuationByte(static_cast<unsigned char>(value[cursor]))) {
        --cursor;
    }
    return cursor;
}

std::size_t Utf8NextCodepointEnd(const std::string& value, std::size_t cursor) {
    cursor = std::min(cursor, value.size());
    if (cursor >= value.size()) {
        return value.size();
    }
    ++cursor;
    while (cursor < value.size() && IsUtf8ContinuationByte(static_cast<unsigned char>(value[cursor]))) {
        ++cursor;
    }
    return cursor;
}

std::size_t DisplayColumns(const std::string& value) {
    std::mbstate_t state{};
    std::size_t columns = 0;
    const char* current = value.data();
    std::size_t remaining = value.size();
    while (remaining > 0) {
        wchar_t wide = 0;
        const std::size_t consumed = std::mbrtowc(&wide, current, remaining, &state);
        if (consumed == static_cast<std::size_t>(-1) || consumed == static_cast<std::size_t>(-2) || consumed == 0) {
            ++columns;
            ++current;
            --remaining;
            state = std::mbstate_t{};
            continue;
        }
        const int width = ::wcwidth(wide);
        columns += width > 0 ? static_cast<std::size_t>(width) : 0;
        current += consumed;
        remaining -= consumed;
    }
    return columns;
}
#endif

void PrintBanner(const std::filesystem::path& workspace) {
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
        << "  jobs                              Show background jobs\n"
        << "  schedule list                     List scheduled tasks\n"
        << "  schedule history                  Show scheduler run history\n"
        << "  help                              Show this help message\n"
        << "  exit / quit                       Exit the interactive console\n"
        << "  exit --wait                       Wait for running background jobs, then exit\n"
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

// REPL chat always dispatches through the "main" adapter — the
// REST-only primary chat agent configured via `agentos main-agent set`.
// Sub-agents (gemini / anthropic / qwen / openai / codex_cli) remain
// available for orchestrated specialist work via `agentos subagents
// run`, but they are not the chat shell. Returns empty if main is
// unhealthy so the caller can print a setup hint.
bool IsCodexOAuthMainAgentConfig(const std::filesystem::path& workspace) {
    const MainAgentStore store(workspace / "runtime" / "main_agent.tsv");
    const auto config = store.load();
    if (!config.has_value()) {
        return false;
    }
    if (config->provider_kind != "openai-chat" || config->oauth_file.empty()) {
        return false;
    }
    const auto oauth_file = config->oauth_file;
    return oauth_file.find(".codex") != std::string::npos ||
           oauth_file.find("codex") != std::string::npos;
}

std::string ResolveChatTarget(const AgentRegistry& agent_registry,
                              const std::filesystem::path& workspace) {
    if (const char* override_target = std::getenv("AGENTOS_CHAT_TARGET")) {
        const std::string target = override_target;
        const auto adapter = agent_registry.find(target);
        if (adapter && adapter->healthy()) {
            return target;
        }
    }

    if (IsCodexOAuthMainAgentConfig(workspace)) {
        const auto adapter = agent_registry.find("codex_cli");
        if (adapter && adapter->healthy()) {
            return "codex_cli";
        }
    }

    const auto adapter = agent_registry.find("main");
    if (adapter && adapter->healthy()) {
        return "main";
    }
    return {};
}

void RunChatPrompt(const std::string& prompt,
                   SkillRegistry& skill_registry,
                   AgentRegistry& agent_registry,
                   AgentLoop& loop,
                   AuditLogger& audit_logger,
                   const std::filesystem::path& workspace,
                   std::vector<ChatTranscriptTurn>* chat_history = nullptr,
                   PendingRouteAction* pending_route_action = nullptr,
                   bool allow_route_actions = true,
                   std::optional<std::string> transcript_user_prompt = std::nullopt) {
    const auto target = ResolveChatTarget(agent_registry, workspace);
    if (target.empty()) {
        std::cerr
            << "main-agent is not configured (or its auth token is unavailable).\n"
            << "Run `agentos main-agent show` to inspect the current record. Examples:\n"
            << "  agentos main-agent set provider=openai-chat \\\n"
            << "    base_url=https://api.openai.com/v1 model=gpt-4o api_key_env=OPENAI_API_KEY\n"
            << "  agentos main-agent set provider=anthropic-messages \\\n"
            << "    base_url=https://api.anthropic.com model=claude-sonnet-4-5 api_key_env=ANTHROPIC_API_KEY\n"
            << "  agentos main-agent set provider=gemini-generatecontent \\\n"
            << "    base_url=https://generativelanguage.googleapis.com/v1beta \\\n"
            << "    model=gemini-2.5-flash api_key_env=GEMINI_API_KEY  # API key, NOT OAuth\n"
            << "  agentos main-agent set provider=vertex-gemini \\\n"
            << "    project_id=<your-gcp-project> location=us-central1 \\\n"
            << "    model=gemini-2.5-flash oauth_file=$HOME/.gemini/oauth_creds.json\n"
            << "Run `agentos main-agent list-providers` for the full reference.\n";
        return;
    }

    // Main agent is REST-only, so there's no provider CLI auto-loading
    // GEMINI.md / CLAUDE.md / AGENTS.md from cwd — the workspace_path
    // is just where the audit log lives and where curl temp files get
    // staged. No scratch dir or override files needed.
    TaskRequest task{
        .task_id = MakeTaskId("interactive-chat"),
        .task_type = "chat",
        .objective = prompt,
        .workspace_path = workspace,
    };
    task.preferred_target = target;
    task.inputs["intent_hint"] =
        "contextual_repl_turn; inspect conversation_context first; answer ordinary continuations, "
        "clarifications, constraint updates, and follow-ups directly; request a registered AgentOS "
        "capability only when the live turn explicitly needs tool, agent, code, research, or file "
        "execution now";
    if (chat_history != nullptr) {
        const auto context = RenderRecentChatContext(*chat_history);
        if (!context.empty()) {
            task.inputs["conversation_context"] = context;
        }
    }
    if (pending_route_action != nullptr && pending_route_action->active && allow_route_actions) {
        task.inputs["pending_route_action"] = RenderPendingRouteActionContext(*pending_route_action);
    }
    // Chat hits an external LLM CLI/REST round-trip, which routinely takes
    // longer than the TaskRequest default of 5000ms. Keep the interactive
    // default aligned with main-agent's default_timeout_ms.
    if (task.timeout_ms <= 5000) {
        task.timeout_ms = kInteractiveChatTimeoutMs;
    }

    std::cout << "(routing to " << target
              << " — 120s ceiling, falls back to gemini/anthropic/openai/qwen on failure; Ctrl-C to cancel)"
              << std::endl;

    const auto result = RunChatWithFallback(task, agent_registry, loop, target);

    // Concise chat output: print just the assistant reply plus a one-line
    // route/duration trailer. The full structured agent_result.v1 JSON is
    // still written to the audit log, so deeper inspection is one
    // `tail runtime/audit.log` away. `run` (verbose) remains the path for
    // users who want the full JSON dump.
    if (result.success) {
        long duration_ms = 0;
        for (const auto& step : result.steps) {
            duration_ms += step.duration_ms;
        }
        if (allow_route_actions) {
            if (const auto action = ParseMainRouteAction(result.summary); action.has_value()) {
                std::cout << "(main requested " << action->action
                          << " target=" << action->target_kind << ":" << action->target << ")\n";
                const auto action_result = ExecuteMainRouteAction(
                    *action, skill_registry, agent_registry, workspace, loop, audit_logger);
                if (pending_route_action != nullptr) {
                    if (!action_result.success && action_result.error_code == "InvalidRouteSkillInput") {
                        pending_route_action->active = true;
                        pending_route_action->action = *action;
                        pending_route_action->error_code = action_result.error_code;
                        pending_route_action->error_message = action_result.error_message;
                    } else {
                        *pending_route_action = {};
                    }
                }
                const auto synthesis_prompt = BuildRouteActionResultPrompt(prompt, *action, action_result);
                RunChatPrompt(synthesis_prompt,
                              skill_registry,
                              agent_registry,
                              loop,
                              audit_logger,
                              workspace,
                              chat_history,
                              pending_route_action,
                              false,
                              prompt);
                return;
            }
        }
        if (result.summary.empty()) {
            // Some adapters (notably codex_cli) finish successfully without
            // producing user-facing text — they emit structured events
            // instead. For an interactive chat session that's effectively
            // useless, so call it out explicitly and steer the user toward
            // a chat-shaped provider instead of leaving them staring at
            // a silent prompt.
            std::cout << "(no text reply from " << target;
            if (duration_ms > 0) {
                std::cout << ", " << duration_ms << "ms";
            }
            std::cout << ")\n";
            std::cout
                << "Hint: " << target << " is an agentic tool, not a chat model.\n"
                << "      Set AGENTOS_CHAT_TARGET=gemini (or anthropic/openai/qwen)\n"
                << "      and ensure that provider is healthy via `agents`.\n"
                << "      Full structured output is in the audit log:\n"
                << "      " << audit_logger.log_path().string() << "\n\n";
            return;
        }
        std::cout << result.summary;
        if (result.summary.back() != '\n') {
            std::cout << '\n';
        }
        std::cout << "(via " << target;
        if (duration_ms > 0) {
            std::cout << ", " << duration_ms << "ms";
        }
        std::cout << ")\n\n";
        if (chat_history != nullptr) {
            AppendChatTranscript(
                *chat_history,
                transcript_user_prompt.value_or(prompt),
                result.summary);
            SaveChatTranscript(
                workspace / "runtime" / "main_agent" / "sessions" / "repl-default.json",
                *chat_history);
        }
    } else {
        std::cerr << "chat failed (" << target << "): "
                  << (result.error_code.empty() ? "<no error_code>" : result.error_code);
        if (!result.error_message.empty()) {
            std::cerr << " — " << result.error_message;
        }
        std::cerr << "\naudit_log: " << audit_logger.log_path().string() << '\n';
    }
}

TaskRunResult ExecuteMainRouteAction(const MainRouteAction& action,
                                     SkillRegistry& skill_registry,
                                     AgentRegistry& agent_registry,
                                     const std::filesystem::path& workspace,
                                     AgentLoop& loop,
                                     AuditLogger& audit_logger) {
    const auto validation = ValidateMainRouteAction(action, skill_registry, agent_registry);
    if (!validation.valid) {
        TaskRunResult result;
        result.success = false;
        result.route_target = action.target;
        result.route_kind = action.target_kind == "agent" ? RouteTargetKind::agent : RouteTargetKind::skill;
        result.error_code = validation.error_code;
        result.error_message = validation.error_message;
        result.summary = validation.error_message;
        std::cout << "audit_log: " << audit_logger.log_path().string() << '\n';
        return result;
    }

    const auto objective = action.brief.empty() ? action.target : action.brief;
    TaskRequest task{
        .task_id = MakeTaskId("main-route"),
        .task_type = action.target_kind == "agent" ? std::string("delegate") : action.target,
        .objective = objective,
        .workspace_path = workspace,
    };
    task.preferred_target = action.target;
    task.idempotency_key = task.task_id;
    task.inputs = action.arguments;
    if (!task.inputs.contains("objective")) {
        task.inputs["objective"] = objective;
    }
    task.inputs["main_route_action"] = action.action;
    task.inputs["main_route_target_kind"] = action.target_kind;
    task.inputs["main_route_target"] = action.target;
    task.timeout_ms = 0;
    task.allow_network = true;
    if (const auto allow = action.arguments.find("allow_high_risk");
        allow != action.arguments.end()) {
        task.allow_high_risk = allow->second == "true" || allow->second == "1" || allow->second == "yes";
    }
    if (const auto approval = action.arguments.find("approval_id");
        approval != action.arguments.end()) {
        task.approval_id = approval->second;
    }

    auto task_cancel = InstallSignalCancellation();
    const auto result = loop.run(task, std::move(task_cancel));
    if (!result.success) {
        PrintResult(result);
    }
    std::cout << "audit_log: " << audit_logger.log_path().string() << '\n';
    return result;
}

struct BackgroundJob {
    std::string id;
    std::string kind;
    std::string objective;
    std::filesystem::path workspace;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::atomic<bool> finished{false};
    bool success = false;
    int duration_ms = 0;
    std::string error_code;
    std::string error_message;
    std::mutex mutex;
    std::thread worker;
};

void ReapFinishedJobs(std::vector<std::shared_ptr<BackgroundJob>>& jobs) {
    for (const auto& job : jobs) {
        if (job && job->finished.load() && job->worker.joinable()) {
            job->worker.join();
        }
    }
}

void ListBackgroundJobs(std::vector<std::shared_ptr<BackgroundJob>>& jobs) {
    ReapFinishedJobs(jobs);
    if (jobs.empty()) {
        std::cout << "No background jobs.\n\n";
        return;
    }
    std::cout << "Background jobs (" << jobs.size() << "):\n";
    for (const auto& job : jobs) {
        if (!job) {
            continue;
        }
        std::lock_guard<std::mutex> lock(job->mutex);
        const auto elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - job->started_at).count());
        std::cout << "  " << job->id
                  << "  kind=" << job->kind
                  << "  state=" << (job->finished.load() ? "finished" : "running")
                  << "  elapsed=" << (elapsed_ms / 1000) << "s"
                  << "  workspace=" << job->workspace.string() << '\n';
        AgentEventRuntimeStore event_store(job->workspace);
        if (const auto task_dir = event_store.find_task_dir(job->id); task_dir.has_value()) {
            std::cout << "    task_dir=" << task_dir->string() << '\n';
            const auto status = event_store.read_latest_status(*task_dir);
            if (status.has_value()) {
                std::cout << "    " << FormatAgentEventStatusSummary(*status) << '\n';
            }
        }
        if (job->finished.load()) {
            std::cout << "    success=" << (job->success ? "true" : "false");
            if (!job->error_code.empty()) {
                std::cout << " error=" << job->error_code;
            }
            if (!job->error_message.empty()) {
                std::cout << " message=" << job->error_message;
            }
            std::cout << '\n';
        }
    }
    std::cout << '\n';
}

UsageSnapshot BuildInteractiveUsageSnapshot(const SkillRegistry& skill_registry,
                                            const AgentRegistry& agent_registry,
                                            const MemoryManager& memory_manager,
                                            const Scheduler& scheduler,
                                            const AuditLogger& audit_logger,
                                            const std::filesystem::path& workspace) {
    UsageSnapshot snapshot;
    snapshot.workspace = workspace;
    snapshot.audit_log = audit_logger.log_path();
    snapshot.scheduled_tasks = scheduler.list().size();
    snapshot.workflow_candidates = memory_manager.workflow_candidates().size();

    snapshot.commands = {
        {"run", "run <task_type> [key=value ...]", "Execute a task through the agent loop", {}},
        {"chat", "chat <text>", "Send free-form text to a chat agent", {}},
        {"agents", "agents", "List registered agent adapters", {}},
        {"skills", "skills", "List registered skills", {}},
        {"jobs", "jobs", "Show background jobs", {}},
    };
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto adapter = agent_registry.find(profile.agent_name);
        snapshot.agents.push_back({
            profile.agent_name,
            profile.description,
            adapter && adapter->healthy(),
            adapter && adapter->healthy() ? "healthy" : "unhealthy",
        });
    }
    for (const auto& manifest : skill_registry.list()) {
        const auto adapter = skill_registry.find(manifest.name);
        snapshot.skills.push_back({
            manifest.name,
            manifest.description,
            adapter && adapter->healthy(),
            adapter && adapter->healthy() ? "healthy" : "unhealthy",
        });
    }
    return snapshot;
}

}  // namespace

namespace {

bool IsRetryableChatError(const std::string& code) {
    return code == "ExternalProcessFailed" || code == "Timeout" ||
           code == "AuthExpired" || code == "AuthUnavailable" ||
           code == "AgentUnavailable" || code == "NotConfigured" ||
           code == "ConfigInvalid";
}

TaskRunResult DispatchChatAttempt(TaskRequest task,
                                  AgentRegistry& agent_registry,
                                  AgentLoop& loop,
                                  const std::string& target) {
    (void)agent_registry;
    task.preferred_target = target;
    if (task.timeout_ms <= 0) {
        task.timeout_ms = kInteractiveChatTimeoutMs;
    }
    auto cancel = InstallSignalCancellation();

    std::atomic<bool> done{false};
    std::mutex cv_m;
    std::condition_variable cv;
    std::thread heartbeat([&] {
        int dots = 0;
        while (true) {
            std::unique_lock<std::mutex> lk(cv_m);
            if (cv.wait_for(lk, std::chrono::seconds(2), [&] { return done.load(); })) {
                break;
            }
            std::cout << "." << std::flush;
            if (++dots >= 15) {
                std::cout << " still waiting..." << std::endl;
                dots = 0;
            }
        }
    });

    const auto result = loop.run(task, std::move(cancel));

    {
        std::lock_guard<std::mutex> lk(cv_m);
        done = true;
    }
    cv.notify_all();
    if (heartbeat.joinable()) heartbeat.join();
    std::cout << std::endl;
    return result;
}

}  // namespace

TaskRunResult RunChatWithFallback(TaskRequest task,
                                  AgentRegistry& agent_registry,
                                  AgentLoop& loop,
                                  const std::string& primary_target) {
    std::vector<std::string> tried;
    auto try_target = [&](const std::string& target) -> TaskRunResult {
        tried.push_back(target);
        return DispatchChatAttempt(task, agent_registry, loop, target);
    };

    auto result = try_target(primary_target);
    if (result.success) return result;

    if (!IsRetryableChatError(result.error_code)) {
        return result;
    }

    static constexpr std::array<const char*, 4> kFallbackOrder = {
        "gemini", "anthropic", "openai", "qwen"};
    std::string last_code = result.error_code.empty() ? "Unknown" : result.error_code;
    for (const auto* candidate : kFallbackOrder) {
        const std::string name = candidate;
        if (name == primary_target) continue;
        if (std::find(tried.begin(), tried.end(), name) != tried.end()) continue;
        const auto adapter = agent_registry.find(name);
        if (!adapter || !adapter->healthy()) continue;

        std::cout << "(fell back to " << name
                  << " after " << (tried.empty() ? primary_target : tried.back())
                  << " failed: " << last_code << ")" << std::endl;
        result = try_target(name);
        if (result.success) return result;
        last_code = result.error_code.empty() ? last_code : result.error_code;
        if (!IsRetryableChatError(result.error_code)) {
            break;
        }
    }

    std::ostringstream tried_csv;
    for (size_t i = 0; i < tried.size(); ++i) {
        if (i != 0) tried_csv << ',';
        tried_csv << tried[i];
    }
    if (!result.error_message.empty()) {
        result.error_message += " ";
    }
    result.error_message += "tried=" + tried_csv.str();
    return result;
}

class TerminalRawMode {
public:
    TerminalRawMode() {
#ifndef _WIN32
        enabled_ = ::isatty(STDIN_FILENO) == 1 && ::tcgetattr(STDIN_FILENO, &original_) == 0;
        if (!enabled_) {
            return;
        }
        termios raw = original_;
        raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            enabled_ = false;
        }
#endif
    }

    ~TerminalRawMode() {
#ifndef _WIN32
        if (enabled_) {
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }
#endif
    }

    bool enabled() const {
        return enabled_;
    }

private:
    bool enabled_ = false;
#ifndef _WIN32
    termios original_{};
#endif
};

void RedrawInputLine(const std::string& prompt,
                     const std::string& line,
                     const std::size_t cursor) {
    std::cout << "\r" << prompt << line << "\x1b[K";
#ifdef _WIN32
    const auto right = line.size() - std::min(cursor, line.size());
#else
    const auto right = DisplayColumns(line.substr(std::min(cursor, line.size())));
#endif
    if (right > 0) {
        std::cout << "\x1b[" << right << "D";
    }
    std::cout << std::flush;
}

std::vector<std::string> LoadReplHistory(const std::filesystem::path& path) {
    constexpr std::size_t kMaxHistoryEntries = 500;
    std::vector<std::string> history;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line == "exit" || line == "quit" || line == "/exit" || line == "/quit") {
            continue;
        }
        if (history.empty() || history.back() != line) {
            history.push_back(line);
        }
    }
    if (history.size() > kMaxHistoryEntries) {
        history.erase(history.begin(), history.end() - static_cast<std::ptrdiff_t>(kMaxHistoryEntries));
    }
    return history;
}

void SaveReplHistory(const std::filesystem::path& path,
                     const std::vector<std::string>& history) {
    constexpr std::size_t kMaxHistoryEntries = 500;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return;
    }
    const auto start = history.size() > kMaxHistoryEntries
        ? history.size() - kMaxHistoryEntries
        : std::size_t{0};
    for (std::size_t index = start; index < history.size(); ++index) {
        if (!history[index].empty()) {
            output << history[index] << '\n';
        }
    }
}

bool ReadInteractiveLine(const std::string& prompt,
                         std::string& line,
                         std::vector<std::string>& history) {
#ifdef _WIN32
    std::cout << prompt << std::flush;
    if (!_isatty(_fileno(stdin))) {
        return static_cast<bool>(std::getline(std::cin, line));
    }

    line.clear();
    std::string draft;
    std::size_t cursor = 0;
    std::size_t history_index = history.size();
    while (true) {
        const int ch = _getch();
        if (ch == '\r' || ch == '\n') {
            std::cout << "\n";
            if (!line.empty() && (history.empty() || history.back() != line)) {
                history.push_back(line);
            }
            return true;
        }
        if (ch == 3) {
            std::cout << "^C\n";
            line.clear();
            return true;
        }
        if (ch == 4 && line.empty()) {
            return false;
        }
        if (ch == 8) {
            if (cursor > 0) {
                line.erase(cursor - 1, 1);
                --cursor;
                RedrawInputLine(prompt, line, cursor);
            }
            continue;
        }
        if (ch == 0 || ch == 224) {
            const int key = _getch();
            if (key == 72) {
                if (!history.empty() && history_index > 0) {
                    if (history_index == history.size()) {
                        draft = line;
                    }
                    --history_index;
                    line = history[history_index];
                    cursor = line.size();
                    RedrawInputLine(prompt, line, cursor);
                }
            } else if (key == 80) {
                if (history_index < history.size()) {
                    ++history_index;
                    line = history_index == history.size() ? draft : history[history_index];
                    cursor = line.size();
                    RedrawInputLine(prompt, line, cursor);
                }
            } else if (key == 75) {
                if (cursor > 0) {
                    --cursor;
                    std::cout << "\x1b[D" << std::flush;
                }
            } else if (key == 77) {
                if (cursor < line.size()) {
                    ++cursor;
                    std::cout << "\x1b[C" << std::flush;
                }
            } else if (key == 71) {
                cursor = 0;
                RedrawInputLine(prompt, line, cursor);
            } else if (key == 79) {
                cursor = line.size();
                RedrawInputLine(prompt, line, cursor);
            } else if (key == 83 && cursor < line.size()) {
                line.erase(cursor, 1);
                RedrawInputLine(prompt, line, cursor);
            }
            continue;
        }
        if (ch >= 32) {
            line.insert(cursor, 1, static_cast<char>(ch));
            ++cursor;
            RedrawInputLine(prompt, line, cursor);
        }
    }
#else
    std::setlocale(LC_CTYPE, "");
    if (::isatty(STDIN_FILENO) != 1) {
        std::cout << prompt << std::flush;
        return static_cast<bool>(std::getline(std::cin, line));
    }

    TerminalRawMode raw_mode;
    if (!raw_mode.enabled()) {
        std::cout << prompt << std::flush;
        return static_cast<bool>(std::getline(std::cin, line));
    }

    line.clear();
    std::string draft;
    std::size_t cursor = 0;
    std::size_t history_index = history.size();
    int pending_utf8_continuations = 0;
    std::cout << prompt << std::flush;

    while (true) {
        char ch = 0;
        if (::read(STDIN_FILENO, &ch, 1) != 1) {
            return false;
        }
        if (ch == '\n' || ch == '\r') {
            std::cout << "\n";
            if (!line.empty() && (history.empty() || history.back() != line)) {
                history.push_back(line);
            }
            return true;
        }
        if (ch == 3) {
            std::cout << "^C\n";
            line.clear();
            return true;
        }
        if (ch == 4) {
            if (line.empty()) {
                return false;
            }
            continue;
        }
        if (ch == 127 || ch == 8) {
            if (cursor > 0) {
                const auto erase_start = pending_utf8_continuations > 0
                    ? cursor - 1
                    : Utf8PreviousCodepointStart(line, cursor);
                line.erase(erase_start, cursor - erase_start);
                cursor = erase_start;
                pending_utf8_continuations = 0;
                RedrawInputLine(prompt, line, cursor);
            }
            continue;
        }
        if (ch == 27) {
            char seq1 = 0;
            char seq2 = 0;
            if (::read(STDIN_FILENO, &seq1, 1) != 1) {
                continue;
            }
            if (seq1 != '[' && seq1 != 'O') {
                continue;
            }
            if (::read(STDIN_FILENO, &seq2, 1) != 1) {
                continue;
            }
            if (seq2 == 'A') {
                if (!history.empty() && history_index > 0) {
                    if (history_index == history.size()) {
                        draft = line;
                    }
                    --history_index;
                    line = history[history_index];
                    cursor = line.size();
                    RedrawInputLine(prompt, line, cursor);
                }
            } else if (seq2 == 'B') {
                if (history_index < history.size()) {
                    ++history_index;
                    line = history_index == history.size() ? draft : history[history_index];
                    cursor = line.size();
                    RedrawInputLine(prompt, line, cursor);
                }
            } else if (seq2 == 'C') {
                if (cursor < line.size()) {
                    cursor = Utf8NextCodepointEnd(line, cursor);
                    RedrawInputLine(prompt, line, cursor);
                }
            } else if (seq2 == 'D') {
                if (cursor > 0) {
                    cursor = Utf8PreviousCodepointStart(line, cursor);
                    RedrawInputLine(prompt, line, cursor);
                }
            } else if (seq2 == 'H') {
                cursor = 0;
                RedrawInputLine(prompt, line, cursor);
            } else if (seq2 == 'F') {
                cursor = line.size();
                RedrawInputLine(prompt, line, cursor);
            } else if (seq2 >= '0' && seq2 <= '9') {
                char terminator = 0;
                (void)::read(STDIN_FILENO, &terminator, 1);
                if (seq2 == '3' && terminator == '~' && cursor < line.size()) {
                    const auto erase_end = Utf8NextCodepointEnd(line, cursor);
                    line.erase(cursor, erase_end - cursor);
                    pending_utf8_continuations = 0;
                    RedrawInputLine(prompt, line, cursor);
                }
            }
            continue;
        }
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 32) {
            line.insert(cursor, 1, ch);
            ++cursor;
            if (pending_utf8_continuations > 0 && IsUtf8ContinuationByte(byte)) {
                --pending_utf8_continuations;
            } else {
                pending_utf8_continuations = Utf8ExpectedContinuationCount(byte);
            }
            if (pending_utf8_continuations == 0) {
                RedrawInputLine(prompt, line, cursor);
            }
        }
    }
#endif
}

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
    ConsoleCodePageGuard console_code_page_guard;

    PrintBanner(workspace);

    auto cancel = InstallSignalCancellation();
    std::vector<std::shared_ptr<BackgroundJob>> background_jobs;
    const auto history_path = workspace / "runtime" / "repl_history.txt";
    std::vector<std::string> line_history = LoadReplHistory(history_path);
    const auto chat_session_path = workspace / "runtime" / "main_agent" / "sessions" / "repl-default.json";
    std::vector<ChatTranscriptTurn> chat_history = LoadChatTranscript(chat_session_path);
    PendingRouteAction pending_route_action;

    std::string line;
    while (true) {
        ReapFinishedJobs(background_jobs);

        // Check if Ctrl-C was pressed between commands.
        if (cancel && cancel->is_cancelled()) {
            std::cout << "\nInterrupted. Exiting interactive console.\n";
            break;
        }

        if (!ReadInteractiveLine("agentos> ", line, line_history)) {
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
        if (!line_history.empty()) {
            line_history.back() = line;
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
            const auto running = std::count_if(background_jobs.begin(), background_jobs.end(),
                [](const std::shared_ptr<BackgroundJob>& job) {
                    return job && !job->finished.load();
                });
            const bool wait_for_jobs = std::find(tokens.begin() + 1, tokens.end(), "--wait") != tokens.end() ||
                std::find(tokens.begin() + 1, tokens.end(), "wait") != tokens.end();
            if (running > 0 && !wait_for_jobs) {
                std::cout << running << " background job(s) still running.\n"
                          << "Use `jobs` to inspect progress, or `exit --wait` to wait for completion.\n\n";
                continue;
            }
            if (!line_history.empty() && line_history.back() == line) {
                line_history.pop_back();
            }
            SaveReplHistory(history_path, line_history);
            if (running > 0 && wait_for_jobs) {
                std::cout << "Waiting for " << running
                          << " background job(s) before exit. Press Ctrl-C again to force terminate.\n";
                for (const auto& job : background_jobs) {
                    if (job && job->worker.joinable()) {
                        job->worker.join();
                    }
                }
            }
            std::cout << "Goodbye.\n";
            break;
        }
        SaveReplHistory(history_path, line_history);

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
            PrintRegisteredAgentsGuide(agent_registry);
            continue;
        }

        // ── skills ──────────────────────────────────────────────────────
        if (command == "skills") {
            PrintRegisteredSkillsGuide(skill_registry, workspace);
            continue;
        }

        // ── status ──────────────────────────────────────────────────────
        if (command == "status") {
            std::cout << "AgentOS Interactive Console Status\n";
            std::cout << "  workspace: " << workspace.string() << '\n';
            std::cout << "  skills: " << skill_registry.list().size() << '\n';
            std::cout << "  agents: " << agent_registry.list_profiles().size() << '\n';
            std::cout << "  background_jobs: " << background_jobs.size() << '\n';
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
            RunChatPrompt(
                prompt,
                skill_registry,
                agent_registry,
                loop,
                audit_logger,
                workspace,
                &chat_history,
                &pending_route_action);
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

        // ── jobs ────────────────────────────────────────────────────────
        if (command == "jobs") {
            ListBackgroundJobs(background_jobs);
            continue;
        }

        const auto usage_snapshot = BuildInteractiveUsageSnapshot(
            skill_registry, agent_registry, memory_manager, scheduler, audit_logger, workspace);
        const auto route_decision = ClassifyInteractiveRequest(
            line,
            skill_registry,
            agent_registry,
            usage_snapshot,
            workspace,
            [&agent_registry, &workspace]() { return ResolveChatTarget(agent_registry, workspace); },
            [&skill_registry]() { return skill_registry.find("development_request") ? "development_request" : ""; },
            [&skill_registry]() { return skill_registry.find("research_request") ? "research_request" : ""; },
            [](const std::string&) { return false; },
            [](const std::string&) { return std::string{}; });
        WriteRouteDecision(workspace, route_decision);
        PrintRouteDecision(route_decision, RuntimeLanguage::English);

        switch (route_decision.route) {
        case InteractiveRouteKind::direct_skill:
            break;
        case InteractiveRouteKind::local_intent:
            if (TryConfigureMainAgentFromNaturalLanguage(line, workspace)) {
                continue;
            }
            if (LooksLikeModelIdentityQuestion(line)) {
                PrintMainModelIdentity(agent_registry, workspace);
                continue;
            }
            if (LooksLikeSpecificSkillUsageQuestion(line)) {
                if (const auto manifest = FindMentionedSkill(line, skill_registry); manifest.has_value()) {
                    PrintSkillUsageGuide(*manifest);
                    continue;
                }
            }
            if (LooksLikeAgentListQuestion(line)) {
                PrintRegisteredAgentsGuide(agent_registry);
                continue;
            }
            if (LooksLikeSkillListQuestion(line)) {
                PrintRegisteredSkillsGuide(skill_registry, workspace);
                continue;
            }
            if (LooksLikeMemoryQuestion(line)) {
                if (LooksLikeDetailedMemoryQuestion(line)) {
                    PrintMemoryDetails(memory_manager);
                } else {
                    PrintMemoryOverview(memory_manager);
                }
                continue;
            }
            if (PrintLocalBrowserErrorFollowup(line, workspace)) {
                continue;
            }
            break;
        case InteractiveRouteKind::development_agent:
        case InteractiveRouteKind::research_agent:
            RunChatPrompt(
                line,
                skill_registry,
                agent_registry,
                loop,
                audit_logger,
                workspace,
                &chat_history,
                &pending_route_action);
            continue;
        case InteractiveRouteKind::chat_agent:
            RunChatPrompt(
                line,
                skill_registry,
                agent_registry,
                loop,
                audit_logger,
                workspace,
                &chat_history,
                &pending_route_action);
            continue;
        case InteractiveRouteKind::unknown_command:
            break;
        }
        RunChatPrompt(
            line,
            skill_registry,
            agent_registry,
            loop,
            audit_logger,
            workspace,
            &chat_history,
            &pending_route_action);
    }

    for (const auto& job : background_jobs) {
        if (job && job->worker.joinable()) {
            job->worker.join();
        }
    }
    return 0;
}

}  // namespace agentos
