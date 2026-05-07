#include "cli/interactive_chat_state.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestAppendChatTranscriptKeepsRecentTurns() {
    std::vector<agentos::ChatTranscriptTurn> history;
    for (int i = 0; i < 8; ++i) {
        agentos::AppendChatTranscript(history, "user-" + std::to_string(i), "assistant-" + std::to_string(i));
    }

    Expect(history.size() == 6, "chat transcript should keep the six most recent turns");
    Expect(history.front().user == "user-2", "chat transcript should drop oldest turns");
    Expect(history.back().assistant == "assistant-7", "chat transcript should keep newest assistant text");
}

void TestRenderRecentChatContext() {
    std::vector<agentos::ChatTranscriptTurn> history;
    agentos::AppendChatTranscript(history, "first", "reply");
    agentos::AppendChatTranscript(history, "second", "");

    const auto rendered = agentos::RenderRecentChatContext(history);
    Expect(rendered.find("[RECENT REPL CHAT CONTEXT]") != std::string::npos,
           "recent chat context should include start marker");
    Expect(rendered.find("User: first") != std::string::npos,
           "recent chat context should include user text");
    Expect(rendered.find("Assistant: reply") != std::string::npos,
           "recent chat context should include assistant text");
    Expect(rendered.find("Assistant: \n") == std::string::npos,
           "recent chat context should skip empty assistant text");
}

void TestRenderPendingRouteActionContext() {
    agentos::PendingRouteAction pending;
    Expect(agentos::RenderPendingRouteActionContext(pending).empty(),
           "inactive pending route action should render empty context");

    pending.active = true;
    pending.action.action = "call_capability";
    pending.action.target_kind = "skill";
    pending.action.target = "news_search";
    pending.action.brief = "GOAL: search news";
    pending.action.arguments["query"] = "AI browser";
    pending.error_code = "InvalidRouteSkillInput";
    pending.error_message = "missing required input fields for news_search: query";

    const auto rendered = agentos::RenderPendingRouteActionContext(pending);
    Expect(rendered.find("[PENDING AGENTOS ROUTE ACTION]") != std::string::npos,
           "pending context should include start marker");
    Expect(rendered.find("\"target\": \"news_search\"") != std::string::npos,
           "pending context should include target");
    Expect(rendered.find("\"query\": \"AI browser\"") != std::string::npos,
           "pending context should include existing arguments");
    Expect(rendered.find("emit a fresh agentos_route_action") != std::string::npos,
           "pending context should instruct main to emit a fresh route action");
}

}  // namespace

int main() {
    TestAppendChatTranscriptKeepsRecentTurns();
    TestRenderRecentChatContext();
    TestRenderPendingRouteActionContext();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
