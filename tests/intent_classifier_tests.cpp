#include "cli/intent_classifier.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

agentos::RouteDecisionExplanation Classify(const std::string& line) {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    agentos::UsageSnapshot usage;
    usage.workspace = std::filesystem::temp_directory_path();

    return agentos::ClassifyInteractiveRequest(
        line,
        skill_registry,
        agent_registry,
        usage,
        usage.workspace,
        [] { return std::string("main"); },
        [] { return std::string("development_request"); },
        [] { return std::string("research_request"); },
        [](const std::string&) { return false; },
        [](const std::string&) { return std::string{}; });
}

void TestResearchFirstSkillLibraryUrlRoutesToResearch() {
    const auto decision = Classify(
        "帮我研究这个项目，如何加入到我们的技能库：https://github.com/maxenergy/skills.git");
    Expect(decision.route == agentos::InteractiveRouteKind::chat_agent,
        "research-shaped natural language should first route to main");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::sync,
        "main-first routing should be synchronous");
    Expect(decision.selected_target == "main",
        "research-shaped natural language should target main");
}

void TestExplicitInstallRoutesToDevelopment() {
    const auto decision = Classify(
        "把 https://github.com/maxenergy/skills.git 安装到 .agents/skills 技能库");
    Expect(decision.route == agentos::InteractiveRouteKind::chat_agent,
        "development-shaped natural language should first route to main");
    Expect(decision.selected_target == "main",
        "development-shaped natural language should target main");
}

void TestArtifactRequestRoutesToDevelopment() {
    const auto decision = Classify("帮我写一个 3 页 PPT 大纲并保存为文件");
    Expect(decision.route == agentos::InteractiveRouteKind::chat_agent,
        "artifact creation request should first route to main");
}

void TestCommandLineToolBuildRoutesToDevelopment() {
    const auto decision = Classify("please build a small command line tool");
    Expect(decision.route == agentos::InteractiveRouteKind::chat_agent,
        "command line tool build request should first route to main");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::sync,
        "main-first build request should run synchronously until main requests a route action");
}

void TestMainAgentOllamaConfigRoutesToLocalIntent() {
    const auto decision = Classify("请帮我配置首选模型使用本地ollama的gemma4:e2b模型。");
    Expect(decision.route == agentos::InteractiveRouteKind::local_intent,
        "main-agent Ollama config should route to local_intent");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::sync,
        "main-agent Ollama config should be synchronous");
    Expect(decision.selected_target == "interactive_runtime",
        "main-agent Ollama config should be handled by the interactive runtime");
}

void TestCurrentModelQuestionRoutesToLocalIntent() {
    const auto decision = Classify("当前模型是什么？");
    Expect(decision.route == agentos::InteractiveRouteKind::local_intent,
        "current model question should route to local_intent");
    Expect(decision.selected_target == "interactive_runtime",
        "current model question should be handled by the interactive runtime");
}

void TestMemoryQuestionRoutesToLocalIntent() {
    const auto decision = Classify("你记得什么？");
    Expect(decision.route == agentos::InteractiveRouteKind::local_intent,
        "memory question should route to local_intent");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::sync,
        "memory question should be synchronous");
    Expect(decision.selected_target == "interactive_runtime",
        "memory question should be handled by the interactive runtime");
}

void TestBusinessDiscussionWithGenerateTermsRoutesToChat() {
    const auto decision = Classify(
        "低频，因为每批处理完成后，会根据画像生成沟通术语，执行完成后到下一批估计都有几小时。");
    Expect(decision.route == agentos::InteractiveRouteKind::chat_agent,
        "business discussion with generated terms should stay on main chat");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::sync,
        "business discussion should remain synchronous");
    Expect(decision.selected_target == "main",
        "business discussion should target main");
}

}  // namespace

int main() {
    TestResearchFirstSkillLibraryUrlRoutesToResearch();
    TestExplicitInstallRoutesToDevelopment();
    TestArtifactRequestRoutesToDevelopment();
    TestCommandLineToolBuildRoutesToDevelopment();
    TestMainAgentOllamaConfigRoutesToLocalIntent();
    TestCurrentModelQuestionRoutesToLocalIntent();
    TestMemoryQuestionRoutesToLocalIntent();
    TestBusinessDiscussionWithGenerateTermsRoutesToChat();

    if (failures != 0) {
        std::cerr << failures << " intent classifier test assertion(s) failed\n";
        return 1;
    }
    std::cout << "intent classifier tests passed\n";
    return 0;
}
