#include "cli/main_route_action.hpp"

#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class RequiredInputSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "required_input",
            .version = "test",
            .description = "Requires a query.",
            .capabilities = {"network"},
            .input_schema_json = R"({"type":"object","required":["query"]})",
            .output_schema_json = "{}",
            .risk_level = "low",
            .permissions = {},
            .idempotent = true,
        };
    }
    agentos::SkillResult execute(const agentos::SkillCall&) override {
        return {
            .success = true,
            .json_output = "{}",
            .error_code = {},
            .error_message = {},
        };
    }
    bool healthy() const override { return true; }
};

class HighRiskSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "dangerous_tool",
            .version = "test",
            .description = "High risk tool.",
            .capabilities = {"filesystem"},
            .input_schema_json = R"({"type":"object"})",
            .output_schema_json = "{}",
            .risk_level = "high",
            .permissions = {},
            .idempotent = true,
        };
    }
    agentos::SkillResult execute(const agentos::SkillCall&) override {
        return {
            .success = true,
            .json_output = "{}",
            .error_code = {},
            .error_message = {},
        };
    }
    bool healthy() const override { return true; }
};

void TestParsesPlainJsonEnvelope() {
    const auto action = agentos::ParseMainRouteAction(R"({
      "agentos_route_action": {
        "action": "call_capability",
        "target_kind": "skill",
        "target": "host_info",
        "brief": "check local host",
        "arguments": {
          "verbose": true,
          "limit": 3,
          "ratio": 1.5,
          "filters": ["ip", "hostname"]
        }
      }
    })");

    Expect(action.has_value(), "plain route action JSON should parse");
    if (!action.has_value()) {
        return;
    }
    Expect(action->action == "call_capability", "action should parse");
    Expect(action->target_kind == "skill", "target_kind should parse");
    Expect(action->target == "host_info", "target should parse");
    Expect(action->brief == "check local host", "brief should parse");
    Expect(action->mode == "sync", "mode should default to sync");
    Expect(action->arguments.at("verbose") == "true", "boolean argument should stringify");
    Expect(action->arguments.at("limit") == "3", "integer argument should stringify");
    Expect(action->arguments.at("filters") == R"(["ip","hostname"])", "array argument should dump as JSON");
}

void TestParsesFencedAndMixedTextEnvelope() {
    const auto fenced = agentos::ParseMainRouteAction(R"(```json
{
  "agentos_route_action": {
    "action": "call_capability",
    "target": "news_search",
    "mode": "sync",
    "arguments": {"query": "latest ai browser"}
  }
}
```)");

    Expect(fenced.has_value(), "fenced route action JSON should parse");
    if (fenced.has_value()) {
        Expect(fenced->target_kind == "skill", "missing target_kind should default to skill");
        Expect(fenced->arguments.at("query") == "latest ai browser", "string argument should parse");
    }

    const auto mixed = agentos::ParseMainRouteAction(R"(I should use a capability:
{"agentos_route_action":{"action":"call_capability","target_kind":"agent","target":"codex_cli","brief":"inspect code"}}
)");
    Expect(mixed.has_value(), "mixed text route action JSON should parse");
    if (mixed.has_value()) {
        Expect(mixed->target_kind == "agent", "mixed text target_kind should parse");
        Expect(mixed->target == "codex_cli", "mixed text target should parse");
    }
}

void TestRejectsInvalidEnvelope() {
    Expect(!agentos::ParseMainRouteAction("normal assistant reply").has_value(),
           "normal assistant reply should not parse as route action");
    Expect(!agentos::ParseMainRouteAction(R"({"agentos_route_action":{"action":"call_capability"}})").has_value(),
           "route action without target should be rejected");
}

void TestBuildsRouteActionResultPrompt() {
    agentos::MainRouteAction action;
    action.action = "call_capability";
    action.target_kind = "skill";
    action.target = "host_info";

    agentos::TaskRunResult result;
    result.success = true;
    result.summary = "host ok";
    result.route_target = "host_info";
    result.output_json = R"({"hostname":"devbox"})";

    const auto prompt = agentos::BuildRouteActionResultPrompt("what host is this?", action, result);
    Expect(prompt.find("[AGENTOS ROUTE ACTION RESULT]") != std::string::npos,
           "result prompt should include start marker");
    Expect(prompt.find("\"original_user_request\": \"what host is this?\"") != std::string::npos,
           "result prompt should include original request");
    Expect(prompt.find("\"target\": \"host_info\"") != std::string::npos,
           "result prompt should include route target");
    Expect(prompt.find("Do not emit another agentos_route_action") != std::string::npos,
           "result prompt should forbid recursive route action");
}

void TestBuildsMissingInputClarificationPrompt() {
    agentos::MainRouteAction action;
    action.action = "call_capability";
    action.target_kind = "skill";
    action.target = "news_search";

    agentos::TaskRunResult result;
    result.success = false;
    result.summary = "missing required input fields for news_search: query";
    result.route_target = "news_search";
    result.error_code = "InvalidRouteSkillInput";
    result.error_message = result.summary;

    const auto prompt = agentos::BuildRouteActionResultPrompt("帮我搜新闻", action, result);
    Expect(prompt.find("\"error_code\": \"InvalidRouteSkillInput\"") != std::string::npos,
           "missing input prompt should include machine-readable error code");
    Expect(prompt.find("ask the user one concise clarification question") != std::string::npos,
           "missing input prompt should instruct main to ask a clarification question");
    Expect(prompt.find("missing field") != std::string::npos,
           "missing input prompt should mention missing fields");
}

void TestValidatesRegisteredSkillInputsAndRisk() {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    skill_registry.register_skill(std::make_shared<RequiredInputSkill>());
    skill_registry.register_skill(std::make_shared<HighRiskSkill>());

    agentos::MainRouteAction action;
    action.action = "call_capability";
    action.target_kind = "skill";
    action.target = "required_input";

    auto validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(!validation.valid, "missing required skill input should be invalid");
    Expect(validation.error_code == "InvalidRouteSkillInput",
           "missing required skill input should use InvalidRouteSkillInput");
    Expect(validation.error_message.find("query") != std::string::npos,
           "missing required skill input error should name query");

    action.arguments["query"] = "ai browser";
    validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(validation.valid, "registered skill with required inputs should be valid");

    action.target = "missing_tool";
    validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(!validation.valid, "unknown skill should be invalid");
    Expect(validation.error_code == "UnknownRouteSkill", "unknown skill should use UnknownRouteSkill");

    action.target = "dangerous_tool";
    validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(!validation.valid, "high-risk skill should require approval by route action validation");
    Expect(validation.error_code == "ApprovalRequired",
           "high-risk skill should use ApprovalRequired");
    Expect(validation.error_message.find("agentos trust approval-request") != std::string::npos,
           "approval-required message should include approval request command");
    Expect(validation.error_message.find("allow_high_risk=true") != std::string::npos,
           "approval-required message should explain retry arguments");

    action.arguments["allow_high_risk"] = "true";
    action.arguments["approval_id"] = "approval-123";
    validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(validation.valid,
           "high-risk skill with explicit approval arguments should pass route validation for PolicyEngine");
}

void TestBuildsApprovalRequiredPrompt() {
    agentos::MainRouteAction action;
    action.action = "call_capability";
    action.target_kind = "skill";
    action.target = "dangerous_tool";

    agentos::TaskRunResult result;
    result.success = false;
    result.summary =
        "approval required for high-risk skill dangerous_tool. Request approval with: "
        "agentos trust approval-request subject=main-route-skill:dangerous_tool";
    result.route_target = "dangerous_tool";
    result.error_code = "ApprovalRequired";
    result.error_message = result.summary;

    const auto prompt = agentos::BuildRouteActionResultPrompt("run risky tool", action, result);
    Expect(prompt.find("\"error_code\": \"ApprovalRequired\"") != std::string::npos,
           "approval prompt should include machine-readable error code");
    Expect(prompt.find("needs explicit user approval") != std::string::npos,
           "approval prompt should instruct main to explain approval requirement");
    Expect(prompt.find("allow_high_risk=true") != std::string::npos,
           "approval prompt should explain retry flag");
}

void TestValidatesActionShape() {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;

    agentos::MainRouteAction action;
    action.action = "explain_only";
    action.target_kind = "skill";
    action.target = "host_info";
    auto validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(!validation.valid, "unsupported route action should be invalid");
    Expect(validation.error_code == "UnsupportedRouteAction",
           "unsupported route action should use UnsupportedRouteAction");

    action.action = "call_capability";
    action.target_kind = "workflow";
    validation = agentos::ValidateMainRouteAction(action, skill_registry, agent_registry);
    Expect(!validation.valid, "unsupported target_kind should be invalid");
    Expect(validation.error_code == "UnsupportedRouteTargetKind",
           "unsupported target_kind should use UnsupportedRouteTargetKind");
}

}  // namespace

int main() {
    TestParsesPlainJsonEnvelope();
    TestParsesFencedAndMixedTextEnvelope();
    TestRejectsInvalidEnvelope();
    TestBuildsRouteActionResultPrompt();
    TestBuildsMissingInputClarificationPrompt();
    TestValidatesRegisteredSkillInputsAndRisk();
    TestBuildsApprovalRequiredPrompt();
    TestValidatesActionShape();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
