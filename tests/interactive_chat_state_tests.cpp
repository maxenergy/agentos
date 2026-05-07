#include "cli/interactive_chat_state.hpp"
#include "cli/interactive_main_context.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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
    Expect(rendered.find("[REPL CONTEXT DIGEST]") != std::string::npos,
           "recent chat context should include digest start marker");
    Expect(rendered.find("turn_count: 2") != std::string::npos,
           "recent chat context should include turn count");
    Expect(rendered.find("user_summary: first") != std::string::npos,
           "recent chat context should include sanitized user summary");
    Expect(rendered.find("assistant_summary: reply") != std::string::npos,
           "recent chat context should include sanitized assistant summary");
    Expect(rendered.find("assistant_summary: \n") == std::string::npos,
           "recent chat context should skip empty assistant text");
}

void TestRenderRecentChatContextPrivacyLevels() {
    std::vector<agentos::ChatTranscriptTurn> history;
    agentos::AppendChatTranscript(history, "first", "reply");

    const auto none = agentos::RenderRecentChatContext(
        history,
        agentos::ContextPrivacyLevel::none);
    Expect(none.empty(), "privacy=none should not render prior chat context");

    const auto verbatim = agentos::RenderRecentChatContext(
        history,
        agentos::ContextPrivacyLevel::verbatim);
    Expect(verbatim.find("[RECENT REPL CHAT CONTEXT]") != std::string::npos,
           "privacy=verbatim should use provider-message-compatible transcript markers");
    Expect(verbatim.find("User: first") != std::string::npos,
           "privacy=verbatim should include user text");
    Expect(verbatim.find("Assistant: reply") != std::string::npos,
           "privacy=verbatim should include assistant text");
}

void TestContextPrivacyLevelParsing() {
    Expect(agentos::ParseContextPrivacyLevel("none") == agentos::ContextPrivacyLevel::none,
           "privacy parser should accept none");
    Expect(agentos::ParseContextPrivacyLevel("verbatim") == agentos::ContextPrivacyLevel::verbatim,
           "privacy parser should accept verbatim");
    Expect(agentos::ParseContextPrivacyLevel("digest") == agentos::ContextPrivacyLevel::digest,
           "privacy parser should accept digest");
    Expect(agentos::ParseContextPrivacyLevel("unknown") == agentos::ContextPrivacyLevel::digest,
           "privacy parser should default unknown values to digest");
}

void TestRenderRecentChatContextSanitizesSensitiveDetails() {
    std::vector<agentos::ChatTranscriptTurn> history;
    agentos::AppendChatTranscript(
        history,
        "访问 https://example.test/path?secret=abcdef1234567890abcdef1234567890 并讨论反爬虫细节",
        "token abcdef1234567890abcdef1234567890abcdef1234567890");

    const auto rendered = agentos::RenderRecentChatContext(history);
    Expect(rendered.find("https://example.test") == std::string::npos,
           "recent chat context digest should redact URLs");
    Expect(rendered.find("abcdef1234567890abcdef1234567890") == std::string::npos,
           "recent chat context digest should redact opaque IDs");
    Expect(rendered.find("反爬虫") == std::string::npos,
           "recent chat context digest should redact automation risk phrasing");
    Expect(rendered.find("[url]") != std::string::npos,
           "recent chat context digest should leave a URL placeholder");
    Expect(rendered.find("[automation-risk-detail]") != std::string::npos,
           "recent chat context digest should leave a risk-detail placeholder");
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

void TestChatTranscriptPersistsRecentTurns() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() /
        ("agentos-chat-transcript-test-" + suffix + ".json");
    std::filesystem::remove(path);

    std::vector<agentos::ChatTranscriptTurn> history;
    for (int i = 0; i < 8; ++i) {
        agentos::AppendChatTranscript(history, "persist-user-" + std::to_string(i), "persist-assistant-" + std::to_string(i));
    }
    agentos::SaveChatTranscript(path, history);

    const auto loaded = agentos::LoadChatTranscript(path);
    Expect(loaded.size() == 6, "persisted chat transcript should keep the six most recent turns");
    Expect(!loaded.empty() && loaded.front().user == "persist-user-2",
           "persisted chat transcript should drop older turns");
    Expect(!loaded.empty() && loaded.back().assistant == "persist-assistant-7",
           "persisted chat transcript should reload the newest assistant text");

    std::filesystem::remove(path);
}

void TestMalformedChatTranscriptLoadsEmpty() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto path = std::filesystem::temp_directory_path() /
        ("agentos-chat-transcript-malformed-" + suffix + ".json");
    {
        std::ofstream output(path, std::ios::binary);
        output << "{not json\n";
    }
    const auto loaded = agentos::LoadChatTranscript(path);
    Expect(loaded.empty(), "malformed persisted chat transcript should load as empty history");
    std::filesystem::remove(path);
}

void TestRoutingTraceFormatting() {
    const auto request = agentos::FormatRoutingTraceLine(
        R"({"event":"main_request","task_id":"one","target":"main","context_privacy":"digest","conversation_context_sent":true,"pending_route_action_sent":false,"allow_route_actions":true})");
    Expect(request == "main_request task=one target=main privacy=digest context=true pending=false route_actions=true",
           "routing trace pretty formatter should format main_request records");

    const auto response = agentos::FormatRoutingTraceLine(
        R"({"event":"main_response","task_id":"two","success":true,"route_action_requested":true,"route_action_target_kind":"skill","route_action_target":"host_info","duration_ms":42})");
    Expect(response == "main_response task=two success=true route_action=true target=skill:host_info duration_ms=42",
           "routing trace pretty formatter should format main_response records");

    const auto action = agentos::FormatRoutingTraceLine(
        R"({"event":"route_action_result","task_id":"three","target_kind":"skill","target":"host_info","success":false,"pending_after_action":true,"error_code":"InvalidRouteSkillInput"})");
    Expect(action == "route_action_result task=three target=skill:host_info success=false pending_after=true error=InvalidRouteSkillInput",
           "routing trace pretty formatter should format route_action_result records");

    Expect(agentos::FormatRoutingTraceLine("{not json") == "{not json",
           "routing trace pretty formatter should preserve malformed lines");
}

void TestMainContextHelpers() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto workspace = std::filesystem::temp_directory_path() /
        ("agentos-main-context-test-" + suffix);
    std::filesystem::remove_all(workspace);

    Expect(agentos::IsValidContextName("alpha_1.test"),
           "context name validator should accept safe names");
    Expect(!agentos::IsValidContextName("../bad"),
           "context name validator should reject path-like names");
    Expect(agentos::LoadCurrentMainContextName(workspace) == "repl-default",
           "missing current context should default to repl-default");

    agentos::SaveCurrentMainContextName(workspace, "alpha");
    Expect(agentos::LoadCurrentMainContextName(workspace) == "alpha",
           "current context helper should persist selected context");

    Expect(agentos::LoadMainContextPrivacy(workspace, "alpha") == agentos::ContextPrivacyLevel::digest,
           "missing privacy file should default to digest");
    agentos::SaveMainContextPrivacy(workspace, "alpha", agentos::ContextPrivacyLevel::none);
    Expect(agentos::LoadMainContextPrivacy(workspace, "alpha") == agentos::ContextPrivacyLevel::none,
           "privacy helper should persist per-context privacy");

    agentos::AppendMainRoutingTrace(workspace, {{"event", "main_request"}, {"task_id", "trace-1"}});
    const auto lines = agentos::TailTextFile(agentos::MainRoutingTracePath(workspace), 1);
    Expect(lines.size() == 1 && lines.front().find("\"task_id\":\"trace-1\"") != std::string::npos,
           "routing trace helper should append and tail trace records");

    std::filesystem::remove_all(workspace);
}

}  // namespace

int main() {
    TestAppendChatTranscriptKeepsRecentTurns();
    TestRenderRecentChatContext();
    TestRenderRecentChatContextPrivacyLevels();
    TestContextPrivacyLevelParsing();
    TestRenderRecentChatContextSanitizesSensitiveDetails();
    TestRenderPendingRouteActionContext();
    TestChatTranscriptPersistsRecentTurns();
    TestMalformedChatTranscriptLoadsEmpty();
    TestRoutingTraceFormatting();
    TestMainContextHelpers();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
