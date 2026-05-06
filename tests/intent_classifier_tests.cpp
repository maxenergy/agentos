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
    Expect(decision.route == agentos::InteractiveRouteKind::research_agent,
        "research-first skill-library URL should route to research_agent");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::async_job,
        "research-first skill-library URL should still run as async job");
    Expect(decision.selected_target == "research_request",
        "research-first skill-library URL should target research_request");
}

void TestExplicitInstallRoutesToDevelopment() {
    const auto decision = Classify(
        "把 https://github.com/maxenergy/skills.git 安装到 .agents/skills 技能库");
    Expect(decision.route == agentos::InteractiveRouteKind::development_agent,
        "explicit repo installation should route to development_agent");
    Expect(decision.selected_target == "development_request",
        "explicit repo installation should target development_request");
}

void TestArtifactRequestRoutesToDevelopment() {
    const auto decision = Classify("帮我写一个 3 页 PPT 大纲并保存为文件");
    Expect(decision.route == agentos::InteractiveRouteKind::development_agent,
        "artifact creation request should route to development_agent");
}

void TestCommandLineToolBuildRoutesToDevelopment() {
    const auto decision = Classify("please build a small command line tool");
    Expect(decision.route == agentos::InteractiveRouteKind::development_agent,
        "command line tool build request should route to development_agent");
    Expect(decision.execution_mode == agentos::InteractiveExecutionMode::async_job,
        "command line tool build request should run as async job");
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

}  // namespace

int main() {
    TestResearchFirstSkillLibraryUrlRoutesToResearch();
    TestExplicitInstallRoutesToDevelopment();
    TestArtifactRequestRoutesToDevelopment();
    TestCommandLineToolBuildRoutesToDevelopment();
    TestMainAgentOllamaConfigRoutesToLocalIntent();
    TestCurrentModelQuestionRoutesToLocalIntent();
    TestMemoryQuestionRoutesToLocalIntent();

    if (failures != 0) {
        std::cerr << failures << " intent classifier test assertion(s) failed\n";
        return 1;
    }
    std::cout << "intent classifier tests passed\n";
    return 0;
}
