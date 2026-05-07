#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "hosts/agents/main_agent.hpp"

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

class FsSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "fs_probe",
            .version = "test",
            .description = "Read or write a workspace file. Includes optional patching.",
            .capabilities = {"filesystem", "read"},
            .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path"]})",
            .output_schema_json = "{}",
            .risk_level = "medium",
            .idempotent = true,
        };
    }
    agentos::SkillResult execute(const agentos::SkillCall&) override {
        return {.success = true, .json_output = "{}"};
    }
    bool healthy() const override { return true; }
};

class NetSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "net_probe",
            .version = "test",
            .description = "Fetch a URL via HTTP and return the body. Tracks status codes too.",
            .capabilities = {"network"},
            .input_schema_json = R"({"type":"object","properties":{"url":{"type":"string"}},"required":["url"]})",
            .output_schema_json = "{}",
            .risk_level = "low",
            .idempotent = false,
        };
    }
    agentos::SkillResult execute(const agentos::SkillCall&) override {
        return {.success = true, .json_output = "{}"};
    }
    bool healthy() const override { return true; }
};

class HostSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "host_probe",
            .version = "test",
            .description = "Report local hostname and IP addresses for the current machine.",
            .capabilities = {"host", "introspection"},
            .input_schema_json = R"({"type":"object","properties":{}})",
            .output_schema_json = "{}",
            .risk_level = "low",
            .idempotent = true,
        };
    }
    agentos::SkillResult execute(const agentos::SkillCall&) override {
        return {.success = true, .json_output = "{}"};
    }
    bool healthy() const override { return true; }
};

// Returns the substring of `prompt` that covers a single skill block:
// from the "  <name> [risk=" header to the next skill header (a line
// starting with two spaces followed by a non-space char that is NOT a
// continuation of the same block — continuation lines start with 6+
// spaces). If the skill is the last one, returns to end-of-string.
std::string BlockForSkill(const std::string& prompt, const std::string& skill_name) {
    const auto pos = prompt.find("  " + skill_name + " [risk=");
    if (pos == std::string::npos) return {};
    std::size_t cursor = pos + 1;
    while (true) {
        const auto nl = prompt.find('\n', cursor);
        if (nl == std::string::npos) {
            return prompt.substr(pos);
        }
        cursor = nl + 1;
        if (cursor + 2 < prompt.size() &&
            prompt[cursor] == ' ' && prompt[cursor + 1] == ' ' &&
            prompt[cursor + 2] != ' ') {
            return prompt.substr(pos, nl - pos);
        }
    }
}

bool LineForSkillContains(const std::string& prompt,
                          const std::string& skill_name,
                          const std::string& needle) {
    const auto block = BlockForSkill(prompt, skill_name);
    return !block.empty() && block.find(needle) != std::string::npos;
}

void TestChatPromptIncludesUseWhenAndRequired() {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    skill_registry.register_skill(std::make_shared<FsSkill>());
    skill_registry.register_skill(std::make_shared<NetSkill>());
    skill_registry.register_skill(std::make_shared<HostSkill>());

    agentos::AgentTask task;
    task.task_id = "main-prompt-test";
    task.task_type = "chat";
    task.objective = "what skills do you have?";

    const auto prompt = agentos::BuildMainAgentPrompt(task, &skill_registry, &agent_registry);

    Expect(prompt.find("Registered skills (3):") != std::string::npos,
           "prompt should announce 3 registered skills");

    Expect(LineForSkillContains(prompt, "fs_probe", "use when: you need to read/write a workspace file"),
           "fs_probe line should carry filesystem use-when hint");
    Expect(LineForSkillContains(prompt, "fs_probe", "required: path"),
           "fs_probe line should list 'path' as required");
    Expect(LineForSkillContains(prompt, "fs_probe", "[risk=medium]"),
           "fs_probe line should include risk=medium");
    Expect(LineForSkillContains(prompt, "fs_probe", "Read or write a workspace file"),
           "fs_probe line should include first sentence of description");

    Expect(LineForSkillContains(prompt, "net_probe", "use when: you need to fetch external content"),
           "net_probe line should carry network use-when hint");
    Expect(LineForSkillContains(prompt, "net_probe", "required: url"),
           "net_probe line should list 'url' as required");

    Expect(LineForSkillContains(prompt, "host_probe", "use when: the user is asking about this machine"),
           "host_probe line should carry host/introspection use-when hint");
    Expect(LineForSkillContains(prompt, "host_probe", "required: (none)"),
           "host_probe line should report no required inputs");
}

void TestRouteHintsComeFromCapabilityDeclarationFields() {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    skill_registry.register_skill(std::make_shared<FsSkill>());
    skill_registry.register_skill(std::make_shared<NetSkill>());

    agentos::AgentTask task;
    task.task_id = "route-hint-source";
    task.task_type = "chat";
    task.objective = "which tool should fetch a URL?";

    const auto prompt = agentos::BuildMainAgentPrompt(task, &skill_registry, &agent_registry);

    Expect(LineForSkillContains(prompt, "fs_probe", "use when: you need to read/write a workspace file"),
           "route hint should be derived from the filesystem capability declaration");
    Expect(LineForSkillContains(prompt, "net_probe", "use when: you need to fetch external content"),
           "route hint should be derived from the network capability declaration");
    Expect(LineForSkillContains(prompt, "net_probe", "required: url"),
           "route hint context should include required inputs from input_schema_json");
    Expect(prompt.find("skill_routes.tsv") == std::string::npos,
           "main-agent prompt should not mention or depend on a separate route-hint TSV");
}

void TestNonChatTaskDoesNotEmitSkillCatalog() {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    skill_registry.register_skill(std::make_shared<FsSkill>());

    agentos::AgentTask task;
    task.task_id = "non-chat";
    task.task_type = "analysis";
    task.objective = "analyze something";

    const auto prompt = agentos::BuildMainAgentPrompt(task, &skill_registry, &agent_registry);
    Expect(prompt.find("Registered skills") == std::string::npos,
           "non-chat task_type should NOT emit the skills catalog");
    Expect(prompt.find("Task type: analysis") != std::string::npos,
           "non-chat task_type should still emit the orchestration scaffold");
}

void TestUnknownSchemaProducesFallbackTag() {
    class WeirdSkill final : public agentos::ISkillAdapter {
    public:
        agentos::SkillManifest manifest() const override {
            return {
                .name = "weird_skill",
                .version = "test",
                .description = "Does something",
                .capabilities = {"misc"},
                .input_schema_json = "{not valid json",
                .risk_level = "low",
            };
        }
        agentos::SkillResult execute(const agentos::SkillCall&) override {
            return {.success = true, .json_output = "{}"};
        }
        bool healthy() const override { return true; }
    };

    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    skill_registry.register_skill(std::make_shared<WeirdSkill>());

    agentos::AgentTask task;
    task.task_id = "weird-prompt";
    task.task_type = "chat";
    task.objective = "x";

    const auto prompt = agentos::BuildMainAgentPrompt(task, &skill_registry, &agent_registry);
    Expect(LineForSkillContains(prompt, "weird_skill", "required: (unknown)"),
           "skill with unparseable schema should report required: (unknown)");
}

void TestChatPromptIncludesRecentReplContext() {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;

    agentos::AgentTask task;
    task.task_id = "chat-context";
    task.task_type = "chat";
    task.objective = "补充一下节奏：每批处理完成后，下一批通常间隔几个小时。";
    task.context_json = R"({"conversation_context":"[REPL CONTEXT DIGEST]\nturn_count: 1\n- user_summary: 这个批处理方案应该如何安排？\n  assistant_summary: 建议把批处理节奏、交付格式和后续处理边界写清楚。\nrouting_guidance: Treat the live turn as a possible continuation of this digest.\n[END REPL CONTEXT DIGEST]"})";

    const auto prompt = agentos::BuildMainAgentPrompt(task, &skill_registry, &agent_registry);
    Expect(prompt.find("primary conversational orchestrator") != std::string::npos,
           "main-agent prompt should frame main as the conversational orchestrator");
    Expect(prompt.find("agentos_route_action") != std::string::npos,
           "main-agent prompt should include the structured route action contract");
    Expect(prompt.find("REPL CONTEXT DIGEST") != std::string::npos,
           "main-agent prompt should include recent REPL digest when provided");
    Expect(prompt.find("补充一下节奏") != std::string::npos,
           "main-agent prompt should keep the live user turn separate from context");
    Expect(prompt.find("continuation of the prior topic") != std::string::npos,
           "main-agent prompt should make contextual continuation the first routing question");
    Expect(prompt.find("Do not delegate merely because") != std::string::npos,
           "main-agent prompt should avoid keyword-style delegation from normal conversation text");
}

}  // namespace

int main() {
    TestChatPromptIncludesUseWhenAndRequired();
    TestRouteHintsComeFromCapabilityDeclarationFields();
    TestNonChatTaskDoesNotEmitSkillCatalog();
    TestUnknownSchemaProducesFallbackTag();
    TestChatPromptIncludesRecentReplContext();

    if (failures != 0) {
        std::cerr << failures << " main_agent_prompt test assertion(s) failed\n";
        return 1;
    }
    std::cout << "agentos_main_agent_prompt_tests passed\n";
    return 0;
}
