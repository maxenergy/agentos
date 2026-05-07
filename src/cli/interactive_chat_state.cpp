#include "cli/interactive_chat_state.hpp"

#include "utils/atomic_file.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace agentos {

namespace {

constexpr std::size_t kMaxChatContextTurns = 6;

std::string ShortenCopy(const std::string& text, const std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "...";
}

std::string SanitizeContextText(std::string text) {
    text = std::regex_replace(text, std::regex(R"((https?://|www\.)\S+)"), "[url]");
    text = std::regex_replace(
        text,
        std::regex(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})"),
        "[email]");
    text = std::regex_replace(text, std::regex(R"([A-Za-z0-9_\-]{32,})"), "[opaque-id]");
    text = std::regex_replace(text, std::regex(R"(\b\d{8,}\b)"), "[number]");

    const std::vector<std::pair<std::string, std::string>> sensitive_terms{
        {"反爬虫", "[automation-risk-detail]"},
        {"反爬", "[automation-risk-detail]"},
        {"爬虫", "[automation-risk-detail]"},
        {"抓取", "[data-collection]"},
        {"验证码", "[verification-challenge]"},
        {"风控", "[risk-control]"},
        {"绕过", "[bypass-detail]"},
        {"cookie", "[browser-state]"},
        {"Cookie", "[browser-state]"},
        {"token", "[credential-ref]"},
        {"Token", "[credential-ref]"},
    };
    for (const auto& [from, to] : sensitive_terms) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return text;
}

}  // namespace

std::string ContextPrivacyLevelName(const ContextPrivacyLevel level) {
    switch (level) {
    case ContextPrivacyLevel::digest:
        return "digest";
    case ContextPrivacyLevel::none:
        return "none";
    case ContextPrivacyLevel::verbatim:
        return "verbatim";
    }
    return "digest";
}

ContextPrivacyLevel ParseContextPrivacyLevel(const std::string_view value) {
    if (value == "none") {
        return ContextPrivacyLevel::none;
    }
    if (value == "verbatim") {
        return ContextPrivacyLevel::verbatim;
    }
    return ContextPrivacyLevel::digest;
}

std::string RenderRecentChatContext(const std::vector<ChatTranscriptTurn>& history,
                                    const ContextPrivacyLevel privacy) {
    if (history.empty()) {
        return {};
    }
    if (privacy == ContextPrivacyLevel::none) {
        return {};
    }
    const auto start = history.size() > kMaxChatContextTurns
        ? history.size() - kMaxChatContextTurns
        : 0;
    std::ostringstream out;
    if (privacy == ContextPrivacyLevel::verbatim) {
        out << "[RECENT REPL CHAT CONTEXT]\n";
        for (std::size_t i = start; i < history.size(); ++i) {
            out << "User: " << history[i].user << "\n";
            if (!history[i].assistant.empty()) {
                out << "Assistant: " << ShortenCopy(history[i].assistant, 1200) << "\n";
            }
        }
        out << "[END RECENT REPL CHAT CONTEXT]";
        return out.str();
    }

    out << "[REPL CONTEXT DIGEST]\n"
        << "turn_count: " << history.size() << "\n"
        << "note: This is a sanitized continuity digest, not the full local transcript.\n";
    for (std::size_t i = start; i < history.size(); ++i) {
        out << "- user_summary: " << ShortenCopy(SanitizeContextText(history[i].user), 360) << "\n";
        if (!history[i].assistant.empty()) {
            out << "  assistant_summary: "
                << ShortenCopy(SanitizeContextText(history[i].assistant), 480) << "\n";
        }
    }
    out << "routing_guidance: Treat the live turn as a possible continuation of this digest; "
           "if the digest is insufficient, ask a clarifying question before delegating.\n"
        << "[END REPL CONTEXT DIGEST]";
    return out.str();
}

void AppendChatTranscript(std::vector<ChatTranscriptTurn>& history,
                          std::string user,
                          std::string assistant) {
    history.push_back({
        .user = std::move(user),
        .assistant = std::move(assistant),
    });
    if (history.size() > kMaxChatContextTurns) {
        history.erase(history.begin(),
                      history.begin() + static_cast<std::ptrdiff_t>(history.size() - kMaxChatContextTurns));
    }
}

std::vector<ChatTranscriptTurn> LoadChatTranscript(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    try {
        const auto json = nlohmann::json::parse(input);
        const auto* turns = &json;
        if (json.is_object() && json.contains("turns") && json["turns"].is_array()) {
            turns = &json["turns"];
        }
        if (!turns->is_array()) {
            return {};
        }
        std::vector<ChatTranscriptTurn> history;
        for (const auto& turn : *turns) {
            if (!turn.is_object()) {
                continue;
            }
            const auto user = turn.value("user", std::string{});
            const auto assistant = turn.value("assistant", std::string{});
            if (!user.empty() || !assistant.empty()) {
                AppendChatTranscript(history, user, assistant);
            }
        }
        return history;
    } catch (...) {
        return {};
    }
}

void SaveChatTranscript(const std::filesystem::path& path,
                        const std::vector<ChatTranscriptTurn>& history) {
    nlohmann::ordered_json turns = nlohmann::ordered_json::array();
    const auto start = history.size() > kMaxChatContextTurns
        ? history.size() - kMaxChatContextTurns
        : 0;
    for (std::size_t i = start; i < history.size(); ++i) {
        turns.push_back(nlohmann::ordered_json{
            {"user", history[i].user},
            {"assistant", history[i].assistant},
        });
    }
    const nlohmann::ordered_json payload{
        {"schema", "agentos.repl_chat_transcript.v1"},
        {"turns", turns},
    };
    WriteFileAtomically(path, payload.dump(2) + "\n");
}

std::string RenderPendingRouteActionContext(const PendingRouteAction& pending) {
    if (!pending.active) {
        return {};
    }
    nlohmann::ordered_json payload;
    payload["action"] = pending.action.action;
    payload["target_kind"] = pending.action.target_kind;
    payload["target"] = pending.action.target;
    payload["brief"] = pending.action.brief;
    payload["mode"] = pending.action.mode;
    payload["error_code"] = pending.error_code;
    payload["error_message"] = pending.error_message;
    payload["arguments"] = pending.action.arguments;

    std::ostringstream out;
    out << "[PENDING AGENTOS ROUTE ACTION]\n"
        << payload.dump(2) << "\n"
        << "[END PENDING AGENTOS ROUTE ACTION]\n"
        << "If the user is supplying missing information for this pending capability, "
           "reuse the same registered target and emit a fresh agentos_route_action "
           "with the completed arguments. Otherwise answer normally.";
    return out.str();
}

}  // namespace agentos
