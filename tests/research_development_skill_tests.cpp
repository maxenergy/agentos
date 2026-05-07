#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "memory/memory_manager.hpp"
#include "skills/builtin/development_skill.hpp"
#include "skills/builtin/research_skill.hpp"
#include "utils/cancellation.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text;
}

std::filesystem::path FreshWorkspace(const std::string& name) {
    const auto workspace = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace / "runtime");
    return workspace;
}

class TestAgent final : public agentos::IAgentAdapter, public agentos::IAgentAdapterV2 {
public:
    enum class Mode {
        Succeed,
        Cancel,
    };

    explicit TestAgent(Mode mode) : mode_(mode) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = "codex_cli",
            .version = "test",
            .description = "Test Codex CLI agent",
            .capabilities = {{"code_reasoning", 100}},
            .supports_session = false,
            .supports_streaming = true,
            .supports_patch = true,
            .supports_subagents = false,
            .supports_network = true,
            .cost_tier = "free",
            .latency_tier = "low",
            .risk_level = "low",
        };
    }

    bool healthy() const override { return true; }
    std::string start_session(const std::string&) override { return ""; }
    void close_session(const std::string&) override {}
    agentos::AgentResult run_task(const agentos::AgentTask&) override {
        return {.success = false, .error_code = "LegacyPathNotExpected"};
    }
    agentos::AgentResult run_task_in_session(const std::string&, const agentos::AgentTask&) override {
        return {.success = false, .error_code = "LegacyPathNotExpected"};
    }
    bool cancel(const std::string&) override { return false; }

    agentos::AgentResult invoke(
        const agentos::AgentInvocation& invocation,
        const agentos::AgentEventCallback& on_event = {}) override {
        ++invoke_count;
        last_task_id = invocation.task_id;
        if (mode_ == Mode::Cancel) {
            if (invocation.cancel) {
                invocation.cancel->cancel();
            }
            return {
                .success = false,
                .summary = "cancelled",
                .error_code = "Cancelled",
                .error_message = "test cancellation",
            };
        }
        if (on_event) {
            agentos::AgentEvent event;
            event.kind = agentos::AgentEvent::Kind::Status;
            event.payload_text = "test status";
            (void)on_event(event);
        }
        return {
            .success = true,
            .summary = "done",
        };
    }

    int invoke_count = 0;
    std::string last_task_id;

private:
    Mode mode_;
};

struct Harness {
    explicit Harness(const std::filesystem::path& workspace)
        : audit(workspace / "runtime" / "audit.log"),
          memory(workspace / "runtime" / "memory"),
          cache(workspace / "runtime" / "cache.tsv"),
          loop(skills, agents, router, policy, audit, memory, cache) {}

    agentos::SkillRegistry skills;
    agentos::AgentRegistry agents;
    agentos::Router router;
    agentos::PolicyEngine policy;
    agentos::AuditLogger audit;
    agentos::MemoryManager memory;
    agentos::ExecutionCache cache;
    agentos::AgentLoop loop;
};

void TestResearchUsesRootTaskIdForRuntimeDir() {
    const auto workspace = FreshWorkspace("agentos_research_skill_root_id_test");
    Harness harness(workspace);
    auto agent = std::make_shared<TestAgent>(TestAgent::Mode::Succeed);
    harness.agents.register_agent(agent);

    agentos::ResearchSkill skill(harness.agents, harness.loop, harness.audit, workspace);
    agentos::SkillCall call;
    call.workspace_id = workspace.string();
    call.arguments["objective"] = "research current integration details";
    call.arguments["root_task_id"] = "research-root-test";

    const auto result = skill.execute(call);
    Expect(result.success, "research skill should succeed through the fake network agent");
    const auto output = nlohmann::json::parse(result.json_output);
    const auto task_dir = std::filesystem::path(output.value("task_dir", std::string{}));
    Expect(task_dir.filename() == "research-root-test",
        "research task dir should use the caller-provided root_task_id");
    Expect(std::filesystem::exists(task_dir / "status.json"),
        "research task dir should contain status.json");
    const auto status = nlohmann::json::parse(ReadTextFile(task_dir / "status.json"));
    Expect(status.value("task_id", std::string{}) == "research-root-test",
        "research status task_id should match the background job id");
    Expect(agent->last_task_id == "research-root-test",
        "research inner agent task id should match the background job id");
}

void TestDevelopmentCancellationStopsRepairLoop() {
    const auto workspace = FreshWorkspace("agentos_development_skill_cancel_test");
    Harness harness(workspace);
    auto agent = std::make_shared<TestAgent>(TestAgent::Mode::Cancel);
    harness.agents.register_agent(agent);

    agentos::DevelopmentSkill skill(harness.agents, harness.loop, harness.audit, workspace);
    agentos::SkillCall call;
    call.workspace_id = workspace.string();
    call.arguments["objective"] = "create a file, then cancel";
    call.arguments["root_task_id"] = "dev-root-test";

    const auto result = skill.execute(call);
    Expect(!result.success, "cancelled development skill should not succeed");
    Expect(result.error_code == "Cancelled",
        "cancelled development skill should preserve Cancelled error_code");
    Expect(agent->invoke_count == 1,
        "cancelled development skill should stop after the first attempt");

    const auto output = nlohmann::json::parse(result.json_output);
    Expect(output.value("attempts", 0) == 1,
        "cancelled development output should report exactly one attempt");
    Expect(output.value("acceptance_status", std::string{}) == "cancelled",
        "cancelled development output should report acceptance_status=cancelled");

    const auto aggregate_path = std::filesystem::path(output.value("aggregate_acceptance_file", std::string{}));
    const auto aggregate = nlohmann::json::parse(ReadTextFile(aggregate_path));
    Expect(aggregate.value("attempt_count", 0) == 1,
        "cancelled aggregate acceptance should contain exactly one attempt");
    Expect(aggregate.value("status", std::string{}) == "cancelled",
        "cancelled aggregate acceptance should preserve cancelled status");
}

}  // namespace

int main() {
    TestResearchUsesRootTaskIdForRuntimeDir();
    TestDevelopmentCancellationStopsRepairLoop();

    if (failures != 0) {
        std::cerr << failures << " research/development skill test assertion(s) failed\n";
        return 1;
    }
    std::cout << "research/development skill tests passed\n";
    return 0;
}
