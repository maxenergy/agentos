#include "cli/interactive_commands.hpp"
#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "memory/memory_manager.hpp"

#include <filesystem>
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

std::filesystem::path FreshWorkspace(const std::string& suffix) {
    const auto workspace = std::filesystem::temp_directory_path() / ("agentos_chat_fallback_tests_" + suffix);
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

struct TestRuntime {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    agentos::Router router;
    agentos::PolicyEngine policy_engine;
    agentos::ExecutionCache execution_cache;
    agentos::AuditLogger audit_logger;
    agentos::MemoryManager memory_manager;
    agentos::AgentLoop loop;

    explicit TestRuntime(const std::filesystem::path& workspace)
        : execution_cache(workspace / "execution_cache.tsv"),
          audit_logger(workspace / "audit.log"),
          memory_manager(workspace / "memory"),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

class FailingAgent final : public agentos::IAgentAdapter {
public:
    explicit FailingAgent(std::string name, bool healthy = true, std::string error_code = "ExternalProcessFailed")
        : name_(std::move(name)), healthy_(healthy), error_code_(std::move(error_code)) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "Failing test agent",
            .capabilities = {{"chat", 100}},
            .cost_tier = "free",
            .latency_tier = "low",
            .risk_level = "low",
        };
    }
    bool healthy() const override { return healthy_; }
    std::string start_session(const std::string&) override { return {}; }
    void close_session(const std::string&) override {}
    agentos::AgentResult run_task(const agentos::AgentTask&) override {
        return {
            .success = false,
            .summary = "",
            .error_code = error_code_,
            .error_message = name_ + " always fails",
        };
    }
    agentos::AgentResult run_task_in_session(const std::string&, const agentos::AgentTask& t) override { return run_task(t); }
    bool cancel(const std::string&) override { return false; }

private:
    std::string name_;
    bool healthy_;
    std::string error_code_;
};

class HealthyAgent final : public agentos::IAgentAdapter {
public:
    explicit HealthyAgent(std::string name) : name_(std::move(name)) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "Healthy test agent",
            .capabilities = {{"chat", 100}},
            .cost_tier = "free",
            .latency_tier = "low",
            .risk_level = "low",
        };
    }
    bool healthy() const override { return true; }
    std::string start_session(const std::string&) override { return {}; }
    void close_session(const std::string&) override {}
    agentos::AgentResult run_task(const agentos::AgentTask&) override {
        return {
            .success = true,
            .summary = name_ + " says hi",
            .duration_ms = 1,
        };
    }
    agentos::AgentResult run_task_in_session(const std::string&, const agentos::AgentTask& t) override { return run_task(t); }
    bool cancel(const std::string&) override { return false; }

private:
    std::string name_;
};

agentos::TaskRequest MakeChatTask(const std::filesystem::path& workspace) {
    agentos::TaskRequest task{
        .task_id = "chat-fallback-test",
        .task_type = "chat",
        .objective = "hi",
        .workspace_path = workspace,
    };
    return task;
}

void TestPrimarySuccessNoFallback() {
    const auto workspace = FreshWorkspace("primary_success");
    TestRuntime runtime(workspace);
    runtime.agent_registry.register_agent(std::make_shared<HealthyAgent>("main"));
    runtime.agent_registry.register_agent(std::make_shared<HealthyAgent>("gemini"));

    const auto result = agentos::RunChatWithFallback(
        MakeChatTask(workspace), runtime.agent_registry, runtime.loop, "main");

    Expect(result.success, "primary success should produce success result");
    Expect(result.route_target == "main", "primary success should route to primary target (got: " + result.route_target + ")");
    Expect(result.summary.find("main says hi") != std::string::npos, "primary summary should be reported");
}

void TestPrimaryFailFallbackSucceeds() {
    const auto workspace = FreshWorkspace("primary_fail_fallback_succeeds");
    TestRuntime runtime(workspace);
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("main"));
    runtime.agent_registry.register_agent(std::make_shared<HealthyAgent>("gemini"));
    runtime.agent_registry.register_agent(std::make_shared<HealthyAgent>("anthropic"));

    const auto result = agentos::RunChatWithFallback(
        MakeChatTask(workspace), runtime.agent_registry, runtime.loop, "main");

    Expect(result.success, "fallback path should ultimately succeed");
    Expect(result.route_target == "gemini",
           "fallback should land on gemini first in priority order (got: " + result.route_target + ")");
    Expect(result.summary.find("gemini says hi") != std::string::npos, "fallback summary should be from gemini");
}

void TestAllUnhealthyTriedListInError() {
    const auto workspace = FreshWorkspace("all_unhealthy");
    TestRuntime runtime(workspace);
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("main"));
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("gemini"));
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("anthropic", false));
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("openai", false));
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("qwen", false));

    const auto result = agentos::RunChatWithFallback(
        MakeChatTask(workspace), runtime.agent_registry, runtime.loop, "main");

    Expect(!result.success, "all-unhealthy chain should fail");
    Expect(result.error_message.find("tried=") != std::string::npos,
           "final error_message should carry tried= list (got: " + result.error_message + ")");
    Expect(result.error_message.find("main") != std::string::npos,
           "tried= list should include the primary target (got: " + result.error_message + ")");
    Expect(result.error_message.find("gemini") != std::string::npos,
           "tried= list should include gemini once attempted (got: " + result.error_message + ")");
}

void TestSkipUnhealthyFallbackCandidate() {
    const auto workspace = FreshWorkspace("skip_unhealthy");
    TestRuntime runtime(workspace);
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("main"));
    runtime.agent_registry.register_agent(std::make_shared<FailingAgent>("gemini", false));
    runtime.agent_registry.register_agent(std::make_shared<HealthyAgent>("anthropic"));

    const auto result = agentos::RunChatWithFallback(
        MakeChatTask(workspace), runtime.agent_registry, runtime.loop, "main");

    Expect(result.success, "should skip unhealthy gemini and land on anthropic");
    Expect(result.route_target == "anthropic",
           "should land on anthropic when gemini is unhealthy (got: " + result.route_target + ")");
}

}  // namespace

int main() {
    TestPrimarySuccessNoFallback();
    TestPrimaryFailFallbackSucceeds();
    TestAllUnhealthyTriedListInError();
    TestSkipUnhealthyFallbackCandidate();

    if (failures != 0) {
        std::cerr << failures << " chat_fallback test assertion(s) failed\n";
        return 1;
    }
    std::cout << "agentos_chat_fallback_tests passed\n";
    return 0;
}
