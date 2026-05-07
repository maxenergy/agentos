#include "cli/intent_classifier.hpp"
#include "cli/interactive_intent_registry.hpp"
#include "cli/interactive_route_policy.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#ifndef AGENTOS_ROUTING_FIXTURES_DIR
#define AGENTOS_ROUTING_FIXTURES_DIR "tests/fixtures"
#endif

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
        [](const std::string& text) {
            return text.find("新闻") != std::string::npos ||
                text.find("news") != std::string::npos ||
                text.find("News") != std::string::npos;
        },
        [](const std::string& text) {
            if (text.find("新闻") != std::string::npos ||
                text.find("news") != std::string::npos ||
                text.find("News") != std::string::npos) {
                return std::string("news_search");
            }
            return std::string{};
        });
}

void TestRoutingFixture(const nlohmann::json& fixture) {
    const auto name = fixture.value("name", std::string("<unnamed>"));
    const auto input = fixture.value("input", std::string{});
    const auto decision = Classify(input);

    const auto actual_route = agentos::RouteKindName(decision.route);
    const auto actual_mode = agentos::ExecutionModeName(decision.execution_mode);
    Expect(actual_route == fixture.value("expected_route", std::string{}),
        name + ": route expected " + fixture.value("expected_route", std::string{}) +
        " but got " + actual_route);
    Expect(actual_mode == fixture.value("expected_mode", std::string{}),
        name + ": mode expected " + fixture.value("expected_mode", std::string{}) +
        " but got " + actual_mode);
    Expect(decision.selected_target == fixture.value("expected_target", std::string{}),
        name + ": target expected " + fixture.value("expected_target", std::string{}) +
        " but got " + decision.selected_target);

    if (fixture.contains("must_not_route") && fixture["must_not_route"].is_string()) {
        const auto forbidden = fixture["must_not_route"].get<std::string>();
        Expect(actual_route != forbidden,
            name + ": route must not be " + forbidden);
    }

    if (fixture.contains("expected_ollama_model") && fixture["expected_ollama_model"].is_string()) {
        const auto expected_model = fixture["expected_ollama_model"].get<std::string>();
        const auto actual_model = agentos::ExtractOllamaModelName(input);
        Expect(actual_model.has_value() && *actual_model == expected_model,
            name + ": expected Ollama model " + expected_model);
    }
}

void TestRoutingEvalFixtures() {
    const auto fixture_path =
        std::filesystem::path(AGENTOS_ROUTING_FIXTURES_DIR) / "routing_cases.jsonl";
    std::ifstream input(fixture_path, std::ios::binary);
    Expect(input.good(), "routing eval fixture should be readable: " + fixture_path.string());
    if (!input.good()) {
        return;
    }

    int case_count = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        ++case_count;
        try {
            TestRoutingFixture(nlohmann::json::parse(line));
        } catch (const std::exception& e) {
            Expect(false, std::string("routing fixture parse/eval failed: ") + e.what());
        }
    }
    Expect(case_count >= 8, "routing eval should include the current regression set");
}

void TestAuthoritativeProposalBypassesRouteTierSignals() {
    agentos::InteractiveRouteProposal proposal;
    proposal.task_id = "proposal-hard-local";
    proposal.user_request = "configure default model";
    proposal.route = agentos::InteractiveRouteKind::local_intent;
    proposal.selected_target = "interactive_runtime";
    proposal.reasons.push_back("configures the local main chat model");
    proposal.authoritative = true;
    proposal.signals.development = true;
    proposal.signals.workspace_mutation = true;
    proposal.signals.artifact = true;

    const auto verdict = agentos::MakeInteractiveRouteVerdict(
        proposal,
        agentos::InteractiveRouteTargets{
            .chat = "main",
            .development = "development_request",
            .research = "research_request",
        });

    Expect(verdict.decision.route == agentos::InteractiveRouteKind::local_intent,
        "authoritative hard-local proposal should not be upgraded to development");
    Expect(verdict.decision.execution_mode == agentos::InteractiveExecutionMode::sync,
        "authoritative hard-local proposal should remain synchronous");
}

void TestNonAuthoritativeResearchProposalCannotBecomeDevelopmentWithoutMutation() {
    agentos::InteractiveRouteProposal proposal;
    proposal.task_id = "proposal-research";
    proposal.user_request = "research how to integrate a skill library";
    proposal.signals.development = true;
    proposal.signals.research = true;
    proposal.signals.workspace_mutation = false;
    proposal.signals.artifact = true;

    const auto verdict = agentos::MakeInteractiveRouteVerdict(
        proposal,
        agentos::InteractiveRouteTargets{
            .chat = "main",
            .development = "development_request",
            .research = "research_request",
        });

    Expect(verdict.decision.route == agentos::InteractiveRouteKind::research_agent,
        "research proposal without workspace mutation should remain research");
    Expect(verdict.decision.selected_target == "research_request",
        "research proposal should target research_request");
}

}  // namespace

int main() {
    TestRoutingEvalFixtures();
    TestAuthoritativeProposalBypassesRouteTierSignals();
    TestNonAuthoritativeResearchProposalCannotBecomeDevelopmentWithoutMutation();

    if (failures != 0) {
        std::cerr << failures << " routing eval assertion(s) failed\n";
        return 1;
    }
    std::cout << "routing eval tests passed\n";
    return 0;
}
