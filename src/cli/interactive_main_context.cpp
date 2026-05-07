#include "cli/interactive_main_context.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace agentos {

namespace {

std::string TraceJsonValue(const nlohmann::json& json,
                           const std::string& key,
                           const std::string& fallback = "") {
    if (!json.contains(key)) {
        return fallback;
    }
    const auto& value = json.at(key);
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        std::ostringstream out;
        out << value.get<double>();
        return out.str();
    }
    return fallback;
}

}  // namespace

std::filesystem::path MainRoutingTracePath(const std::filesystem::path& workspace) {
    return workspace / "runtime" / "main_agent" / "routing_trace.jsonl";
}

void AppendMainRoutingTrace(const std::filesystem::path& workspace,
                            nlohmann::ordered_json event) {
    const auto path = MainRoutingTracePath(workspace);
    std::filesystem::create_directories(path.parent_path());
    event["schema"] = "agentos.main_routing_trace.v1";
    event["recorded_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << event.dump() << '\n';
}

void ClearMainRoutingTrace(const std::filesystem::path& workspace) {
    WriteFileAtomically(MainRoutingTracePath(workspace), "");
}

std::vector<std::string> TailTextFile(const std::filesystem::path& path,
                                      const std::size_t max_lines) {
    std::ifstream input(path, std::ios::binary);
    if (!input || max_lines == 0) {
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(std::move(line));
        if (lines.size() > max_lines) {
            lines.erase(lines.begin());
        }
    }
    return lines;
}

std::optional<std::size_t> ParsePositiveSize(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoul(value, &consumed);
        if (consumed != value.size() || parsed == 0) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

std::string FormatRoutingTraceLine(const std::string& line) {
    try {
        const auto json = nlohmann::json::parse(line);
        const auto event = TraceJsonValue(json, "event", "unknown");
        std::ostringstream out;
        out << event;
        if (json.contains("task_id")) {
            out << " task=" << TraceJsonValue(json, "task_id");
        }
        if (event == "main_request") {
            out << " target=" << TraceJsonValue(json, "target")
                << " privacy=" << TraceJsonValue(json, "context_privacy", "digest")
                << " context=" << TraceJsonValue(json, "conversation_context_sent", "false")
                << " pending=" << TraceJsonValue(json, "pending_route_action_sent", "false")
                << " route_actions=" << TraceJsonValue(json, "allow_route_actions", "false");
        } else if (event == "main_response") {
            out << " success=" << TraceJsonValue(json, "success", "false")
                << " route_action=" << TraceJsonValue(json, "route_action_requested", "false");
            if (json.contains("route_action_target")) {
                out << " target=" << TraceJsonValue(json, "route_action_target_kind")
                    << ":" << TraceJsonValue(json, "route_action_target");
            }
            if (json.contains("duration_ms")) {
                out << " duration_ms=" << TraceJsonValue(json, "duration_ms");
            }
        } else if (event == "route_action_result") {
            out << " target=" << TraceJsonValue(json, "target_kind")
                << ":" << TraceJsonValue(json, "target")
                << " success=" << TraceJsonValue(json, "success", "false")
                << " pending_after=" << TraceJsonValue(json, "pending_after_action", "false");
            if (!TraceJsonValue(json, "error_code").empty()) {
                out << " error=" << TraceJsonValue(json, "error_code");
            }
        }
        return out.str();
    } catch (...) {
        return line;
    }
}

void PrintMainContextSummary(const std::filesystem::path& path,
                             const std::string& session_name,
                             const std::vector<ChatTranscriptTurn>& history) {
    std::cout << "AgentOS main context\n"
              << "  session: " << session_name << '\n'
              << "  path:    " << path.string() << '\n'
              << "  turns:   " << history.size() << '\n';
    if (history.empty()) {
        std::cout << "  (empty)\n\n";
        return;
    }
    for (std::size_t i = 0; i < history.size(); ++i) {
        std::cout << "\n[" << (i + 1) << "] user: " << history[i].user << '\n';
        if (!history[i].assistant.empty()) {
            std::cout << "    assistant: " << history[i].assistant << '\n';
        }
    }
    std::cout << '\n';
}

bool IsValidContextName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](const unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
}

std::filesystem::path MainContextSessionsDir(const std::filesystem::path& workspace) {
    return workspace / "runtime" / "main_agent" / "sessions";
}

std::filesystem::path MainContextCurrentPath(const std::filesystem::path& workspace) {
    return workspace / "runtime" / "main_agent" / "current_context.txt";
}

std::filesystem::path MainContextSessionPath(const std::filesystem::path& workspace,
                                             const std::string& session_name) {
    return MainContextSessionsDir(workspace) / (session_name + ".json");
}

std::filesystem::path MainContextPrivacyDir(const std::filesystem::path& workspace) {
    return workspace / "runtime" / "main_agent" / "privacy";
}

std::filesystem::path MainContextPrivacyPath(const std::filesystem::path& workspace,
                                             const std::string& session_name) {
    return MainContextPrivacyDir(workspace) / (session_name + ".txt");
}

ContextPrivacyLevel LoadMainContextPrivacy(const std::filesystem::path& workspace,
                                           const std::string& session_name) {
    std::ifstream input(MainContextPrivacyPath(workspace, session_name), std::ios::binary);
    if (!input) {
        return ContextPrivacyLevel::digest;
    }
    std::string value;
    std::getline(input, value);
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return ContextPrivacyLevel::digest;
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return ParseContextPrivacyLevel(value.substr(start, end - start + 1));
}

void SaveMainContextPrivacy(const std::filesystem::path& workspace,
                            const std::string& session_name,
                            const ContextPrivacyLevel privacy) {
    WriteFileAtomically(MainContextPrivacyPath(workspace, session_name),
                        ContextPrivacyLevelName(privacy) + "\n");
}

std::string LoadCurrentMainContextName(const std::filesystem::path& workspace) {
    std::ifstream input(MainContextCurrentPath(workspace), std::ios::binary);
    if (!input) {
        return "repl-default";
    }
    std::string name;
    std::getline(input, name);
    const auto start = name.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "repl-default";
    }
    const auto end = name.find_last_not_of(" \t\r\n");
    name = name.substr(start, end - start + 1);
    return IsValidContextName(name) ? name : std::string("repl-default");
}

void SaveCurrentMainContextName(const std::filesystem::path& workspace,
                                const std::string& session_name) {
    WriteFileAtomically(MainContextCurrentPath(workspace), session_name + "\n");
}

void PrintMainContextList(const std::filesystem::path& workspace,
                          const std::string& active_session) {
    const auto sessions_dir = MainContextSessionsDir(workspace);
    std::cout << "AgentOS main contexts\n"
              << "  active: " << active_session << '\n'
              << "  path:   " << sessions_dir.string() << '\n';

    std::vector<std::string> names;
    std::error_code ec;
    if (std::filesystem::exists(sessions_dir, ec) && std::filesystem::is_directory(sessions_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(sessions_dir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                continue;
            }
            const auto name = entry.path().stem().string();
            if (IsValidContextName(name)) {
                names.push_back(name);
            }
        }
    }
    if (std::find(names.begin(), names.end(), active_session) == names.end()) {
        names.push_back(active_session);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    for (const auto& name : names) {
        const auto turns = LoadChatTranscript(MainContextSessionPath(workspace, name));
        std::cout << "  " << (name == active_session ? "* " : "  ")
                  << name << " turns=" << turns.size() << '\n';
    }
    std::cout << '\n';
}

}  // namespace agentos
