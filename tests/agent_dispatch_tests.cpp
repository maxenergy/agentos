#include "core/audit/audit_logger.hpp"
#include "core/orchestration/agent_dispatch.hpp"
#include "core/policy/policy_engine.hpp"
#include "memory/memory_manager.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
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

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_agent_dispatch_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

int CountAuditEvents(const std::filesystem::path& audit_log, const std::string& event_name) {
    std::ifstream input(audit_log);
    int count = 0;
    std::string line;
    const auto needle = R"("event":")" + event_name + R"(")";
    while (std::getline(input, line)) {
        if (line.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
}

class StaticAgent final : public agentos::IAgentAdapter {
public:
    StaticAgent(std::string name, std::string capability, const double estimated_cost)
        : name_(std::move(name)), capability_(std::move(capability)), estimated_cost_(estimated_cost) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "Static dispatch test agent",
            .capabilities = {{capability_, 100}},
            .supports_session = false,
            .supports_streaming = false,
            .supports_patch = false,
            .supports_subagents = false,
            .supports_network = false,
            .cost_tier = "free",
            .latency_tier = "low",
            .risk_level = "low",
        };
    }

    bool healthy() const override { return true; }
    std::string start_session(const std::string&) override { return ""; }
    void close_session(const std::string&) override {}

    agentos::AgentResult run_task(const agentos::AgentTask& task) override {
        legacy_calls_ += 1;
        nlohmann::ordered_json structured_output;
        structured_output["agent"] = name_;
        structured_output["objective"] = task.objective;
        return {
            .success = true,
            .summary = "legacy " + task.objective,
            .structured_output_json = structured_output.dump(),
            .duration_ms = 1,
            .estimated_cost = estimated_cost_,
        };
    }

    agentos::AgentResult run_task_in_session(const std::string&, const agentos::AgentTask& task) override {
        return run_task(task);
    }

    bool cancel(const std::string&) override { return false; }

    int legacy_calls() const { return legacy_calls_; }

private:
    std::string name_;
    std::string capability_;
    double estimated_cost_ = 0.0;
    int legacy_calls_ = 0;
};

class V2DualPathAgent final : public agentos::IAgentAdapter, public agentos::IAgentAdapterV2 {
public:
    V2DualPathAgent(std::string name, std::string capability)
        : name_(std::move(name)), capability_(std::move(capability)) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "V2 dispatch test agent",
            .capabilities = {{capability_, 100}},
            .supports_session = false,
            .supports_streaming = true,
            .supports_patch = false,
            .supports_subagents = false,
            .supports_network = false,
            .cost_tier = "free",
            .latency_tier = "low",
            .risk_level = "low",
        };
    }

    bool healthy() const override { return true; }
    std::string start_session(const std::string&) override { return ""; }
    void close_session(const std::string&) override {}

    agentos::AgentResult run_task(const agentos::AgentTask& task) override {
        legacy_calls_ += 1;
        return {
            .success = true,
            .summary = "legacy " + task.objective,
            .duration_ms = 1,
            .estimated_cost = 0.99,
        };
    }

    agentos::AgentResult run_task_in_session(const std::string&, const agentos::AgentTask& task) override {
        return run_task(task);
    }

    bool cancel(const std::string&) override { return false; }

    agentos::AgentResult invoke(
        const agentos::AgentInvocation& invocation,
        const agentos::AgentEventCallback& /*on_event*/) override {
        v2_calls_ += 1;
        last_context_ = invocation.context;
        agentos::AgentResult result;
        result.success = true;
        result.summary = "v2 " + invocation.objective;
        result.duration_ms = 2;
        result.estimated_cost = 0.99;
        result.usage.cost_usd = 0.42;
        result.usage.turns = 1;
        return result;
    }

    int legacy_calls() const { return legacy_calls_; }
    int v2_calls() const { return v2_calls_; }
    const agentos::StringMap& last_context() const { return last_context_; }

private:
    std::string name_;
    std::string capability_;
    int legacy_calls_ = 0;
    int v2_calls_ = 0;
    agentos::StringMap last_context_;
};

struct DispatchRuntime {
    agentos::PolicyEngine policy_engine;
    agentos::AuditLogger audit_logger;
    agentos::MemoryManager memory_manager;

    explicit DispatchRuntime(const std::filesystem::path& workspace)
        : audit_logger(workspace / "audit.log"), memory_manager(workspace / "memory") {}
};

void TestReturnsStepCandidateWithoutRecordingStep(const std::filesystem::path& workspace) {
    DispatchRuntime runtime(workspace / "candidate");
    auto agent = std::make_shared<V2DualPathAgent>("dispatch_v2", "analysis");

    const auto result = agentos::DispatchAgent(
        agentos::AgentDispatchInput{
            .task = agentos::TaskRequest{
                .task_id = "agent-dispatch-candidate",
                .task_type = "analysis",
                .objective = "exercise dispatch seam",
                .workspace_path = workspace,
            },
            .agent = agent,
            .agent_name = "dispatch_v2",
            .agent_task_id = "agent-dispatch-candidate.dispatch_v2",
            .objective = "[analysis] exercise dispatch seam",
            .invocation_context = {
                {"task_type", "analysis"},
                {"parent_task_id", "agent-dispatch-candidate"},
                {"agent", "dispatch_v2"},
                {"role", "analysis"},
            },
        },
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    Expect(result.success, "AgentDispatch should return a successful Dispatch Result for V2 agents");
    Expect(agent->v2_calls() == 1, "AgentDispatch should prefer V2 invoke() when available");
    Expect(agent->legacy_calls() == 0, "AgentDispatch should not call legacy run_task() after V2 dispatch succeeds");
    Expect(result.step.target_name == "dispatch_v2", "Dispatch Result should carry an agent step candidate");
    Expect(result.step.summary.find("v2 ") != std::string::npos, "Dispatch step candidate should reflect V2 output");
    Expect(result.effective_cost > 0.41 && result.effective_cost < 0.43,
        "Dispatch Result should prefer V2 usage cost over legacy estimated cost");
    Expect(result.step.estimated_cost > 0.41 && result.step.estimated_cost < 0.43,
        "Dispatch step candidate should carry the effective V2 usage cost");
    Expect(CountAuditEvents(runtime.audit_logger.log_path(), "policy") == 1,
        "AgentDispatch should audit the policy decision");
    Expect(CountAuditEvents(runtime.audit_logger.log_path(), "step") == 0,
        "AgentDispatch should not record the final task step");
}

void TestFallsBackToLegacyAdapter(const std::filesystem::path& workspace) {
    DispatchRuntime runtime(workspace / "legacy");
    auto agent = std::make_shared<StaticAgent>("dispatch_legacy", "analysis", 0.33);

    const auto result = agentos::DispatchAgent(
        agentos::AgentDispatchInput{
            .task = agentos::TaskRequest{
                .task_id = "agent-dispatch-legacy",
                .task_type = "analysis",
                .objective = "exercise legacy dispatch",
                .workspace_path = workspace,
            },
            .agent = agent,
            .agent_name = "dispatch_legacy",
            .agent_task_id = "agent-dispatch-legacy.dispatch_legacy",
            .objective = "[analysis] exercise legacy dispatch",
            .context_json = "{}",
        },
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    Expect(result.success, "AgentDispatch should support legacy-only adapters");
    Expect(agent->legacy_calls() == 1, "AgentDispatch should call run_task() for legacy-only adapters");
    Expect(result.step.structured_output_json.find(R"("agent":"dispatch_legacy")") != std::string::npos,
        "legacy Dispatch Result should preserve structured agent output");
    Expect(result.effective_cost > 0.32 && result.effective_cost < 0.34,
        "legacy Dispatch Result should use AgentResult.estimated_cost when V2 usage is unavailable");
}

void TestPolicyDeniedReturnsFailureResult(const std::filesystem::path& workspace) {
    DispatchRuntime runtime(workspace / "policy_denied");
    auto agent = std::make_shared<V2DualPathAgent>("dispatch_denied", "analysis");

    const auto result = agentos::DispatchAgent(
        agentos::AgentDispatchInput{
            .task = agentos::TaskRequest{
                .task_id = "agent-dispatch-denied",
                .task_type = "analysis",
                .objective = "remote dispatch should be denied",
                .workspace_path = workspace,
                .remote_trigger = true,
            },
            .agent = agent,
            .agent_name = "dispatch_denied",
            .agent_task_id = "agent-dispatch-denied.dispatch_denied",
            .objective = "[analysis] remote dispatch should be denied",
        },
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    Expect(!result.success, "policy-denied AgentDispatch should return failure");
    Expect(result.error_code == "PolicyDenied", "policy-denied Dispatch Result should carry PolicyDenied");
    Expect(result.step.error_code == "PolicyDenied", "policy-denied step candidate should carry PolicyDenied");
    Expect(agent->v2_calls() == 0, "policy-denied AgentDispatch should not invoke the V2 adapter");
    Expect(agent->legacy_calls() == 0, "policy-denied AgentDispatch should not fall back to legacy dispatch");
    Expect(CountAuditEvents(runtime.audit_logger.log_path(), "policy") == 1,
        "policy-denied AgentDispatch should still audit the policy decision");
    Expect(CountAuditEvents(runtime.audit_logger.log_path(), "step") == 0,
        "policy-denied AgentDispatch should not record the final task step");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestReturnsStepCandidateWithoutRecordingStep(workspace);
    TestFallsBackToLegacyAdapter(workspace);
    TestPolicyDeniedReturnsFailureResult(workspace);

    if (failures != 0) {
        std::cerr << failures << " agent dispatch test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_agent_dispatch_tests passed\n";
    return 0;
}
