#include "cli/interactive_chat_state.hpp"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace agentos {

namespace {

constexpr std::size_t kMaxChatContextTurns = 6;

std::string ShortenCopy(const std::string& text, const std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "...";
}

}  // namespace

std::string RenderRecentChatContext(const std::vector<ChatTranscriptTurn>& history) {
    if (history.empty()) {
        return {};
    }
    const auto start = history.size() > kMaxChatContextTurns
        ? history.size() - kMaxChatContextTurns
        : 0;
    std::ostringstream out;
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
