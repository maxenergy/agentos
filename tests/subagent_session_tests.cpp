#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/orchestration/subagent_manager.hpp"
#include "core/orchestration/workspace_session.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/local_planning_agent.hpp"
#include "memory/memory_manager.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"
#include "trust/allowlist_store.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_manager.hpp"
#include "trust/trust_policy.hpp"
#include "utils/cancellation.hpp"
#include "utils/signal_cancellation.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string ContentJson(const std::string& content) {
    nlohmann::ordered_json output;
    output["content"] = content;
    return output.dump();
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_subagent_session_tests";
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

struct TrustedTestRuntime {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    agentos::Router router;
    agentos::IdentityManager identity_manager;
    agentos::AllowlistStore allowlist_store;
    agentos::PairingManager pairing_manager;
    agentos::TrustPolicy trust_policy;
    agentos::PolicyEngine policy_engine;
    agentos::ExecutionCache execution_cache;
    agentos::AuditLogger audit_logger;
    agentos::MemoryManager memory_manager;
    agentos::AgentLoop loop;

    explicit TrustedTestRuntime(const std::filesystem::path& workspace)
        : identity_manager(workspace / "trust" / "identities.tsv"),
          allowlist_store(workspace / "trust" / "allowlist.tsv"),
          pairing_manager(allowlist_store),
          trust_policy(allowlist_store),
          policy_engine(agentos::PolicyEngineDependencies{.trust_policy = &trust_policy}),
          execution_cache(workspace / "trusted_execution_cache.tsv"),
          audit_logger(workspace / "trusted_audit.log"),
          memory_manager(workspace / "trusted_memory"),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

class StaticTestAgent final : public agentos::IAgentAdapter {
public:
    StaticTestAgent(std::string name, std::string capability, const double estimated_cost = 0.0)
        : name_(std::move(name)), capability_(std::move(capability)), estimated_cost_(estimated_cost) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "Static test agent",
            .capabilities = {
                {capability_, 100},
            },
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

    bool healthy() const override {
        return true;
    }

    std::string start_session(const std::string& session_config_json) override {
        (void)session_config_json;
        return "";
    }

    void close_session(const std::string& session_id) override {
        (void)session_id;
    }

    agentos::AgentResult run_task(const agentos::AgentTask& task) override {
        nlohmann::ordered_json structured_output;
        structured_output["content"] = name_ + " handled " + task.objective;
        structured_output["agent"] = name_;
        structured_output["provider"] = "static_test";
        structured_output["model"] = "static-model";
        structured_output["task_type"] = task.task_type;

        nlohmann::ordered_json artifact_metadata;
        artifact_metadata["source"] = "static_test";

        return {
            .success = true,
            .summary = name_ + " handled " + task.objective,
            .structured_output_json = structured_output.dump(),
            .artifacts = {
                agentos::AgentArtifact{
                    .type = "text",
                    .uri = "memory://" + task.task_id,
                    .content = name_ + " artifact",
                    .metadata_json = artifact_metadata.dump(),
                },
            },
            .duration_ms = 1,
            .estimated_cost = estimated_cost_,
        };
    }

    agentos::AgentResult run_task_in_session(const std::string& session_id, const agentos::AgentTask& task) override {
        (void)session_id;
        return run_task(task);
    }

    bool cancel(const std::string& task_id) override {
        (void)task_id;
        return false;
    }

private:
    std::string name_;
    std::string capability_;
    double estimated_cost_ = 0.0;
};

class DecompositionTestAgent final : public agentos::IAgentAdapter {
public:
    DecompositionTestAgent(std::string name, bool success, std::string structured_output_json)
        : name_(std::move(name)),
          success_(success),
          structured_output_json_(std::move(structured_output_json)) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "Configurable decomposition test agent",
            .capabilities = {
                {"decomposition", 100},
            },
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

    bool healthy() const override {
        return true;
    }

    std::string start_session(const std::string& session_config_json) override {
        (void)session_config_json;
        return "";
    }

    void close_session(const std::string& session_id) override {
        (void)session_id;
    }

    agentos::AgentResult run_task(const agentos::AgentTask& task) override {
        (void)task;
        return {
            .success = success_,
            .summary = success_ ? "decomposition result" : "decomposition failed",
            .structured_output_json = structured_output_json_,
            .duration_ms = 1,
            .error_code = success_ ? "" : "PlannerFailed",
            .error_message = success_ ? "" : "configured planner failure",
        };
    }

    agentos::AgentResult run_task_in_session(const std::string& session_id, const agentos::AgentTask& task) override {
        (void)session_id;
        return run_task(task);
    }

    bool cancel(const std::string& task_id) override {
        (void)task_id;
        return false;
    }

private:
    std::string name_;
    bool success_ = true;
    std::string structured_output_json_;
};

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void RegisterCore(TrustedTestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void TestSubagentManagerSequentialRun(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("agent_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("agent_b", "review"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-sequential",
            .task_type = "analysis",
            .objective = "coordinate two test agents",
            .workspace_path = workspace,
        },
        {"agent_a", "agent_b"},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "sequential subagent orchestration should succeed");
    Expect(result.steps.size() == 2, "sequential subagent orchestration should record two steps");
    Expect(result.steps[0].target_name == "agent_a", "sequential subagent orchestration should preserve agent order");
    Expect(result.steps[1].target_name == "agent_b", "sequential subagent orchestration should preserve second agent order");
    Expect(result.steps[0].summary.find("[analysis] coordinate two test agents") != std::string::npos,
        "sequential subagent orchestration should assign role-scoped objectives");
    Expect(result.steps[1].summary.find("[review] coordinate two test agents") != std::string::npos,
        "sequential subagent orchestration should infer roles from agent capabilities");
    Expect(result.output_json.find("\"roles\":\"analysis,review\"") != std::string::npos,
        "sequential subagent output should include inferred roles");
    Expect(result.steps[0].structured_output_json.find(R"("content")") != std::string::npos,
        "subagent step should preserve agent structured output");
    Expect(result.output_json.find(R"("agent_outputs")") != std::string::npos,
        "subagent result output should include normalized agent outputs");
    Expect(result.output_json.find(R"("schema_version":"agent_result.v1")") != std::string::npos,
        "subagent result output should include the normalized agent result schema version");
    Expect(result.output_json.find(R"("normalized")") != std::string::npos,
        "subagent result output should include normalized per-agent result fields");
    Expect(result.output_json.find(R"("model":"static-model")") != std::string::npos,
        "subagent normalized output should preserve provider model metadata");
    Expect(result.output_json.find(R"("provider":"static_test")") != std::string::npos,
        "subagent normalized output should preserve provider-specific metadata");
    Expect(result.output_json.find(R"("artifacts":[{)") != std::string::npos,
        "subagent normalized output should preserve agent artifacts");
    Expect(result.output_json.find(R"("metrics":{"duration_ms":1,"estimated_cost":0.0})") != std::string::npos,
        "subagent normalized output should include duration and cost metrics");
    Expect(result.output_json.find(R"("tool_calls":[])") != std::string::npos,
        "subagent normalized output should include an explicit tool_calls array");
    Expect(result.output_json.find(R"("task_type":"analysis")") != std::string::npos,
        "subagent result output should carry structured task_type fields");
    Expect(runtime.memory_manager.agent_stats().contains("agent_a"), "subagent run should update agent_a stats");
    Expect(runtime.memory_manager.agent_stats().contains("agent_b"), "subagent run should update agent_b stats");
}

void TestSubagentManagerUsesExplicitRoles(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_explicit_roles_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("role_agent_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("role_agent_b", "review"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-explicit-roles",
            .task_type = "analysis",
            .objective = "split deterministic work",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"roles", "role_agent_a:planner,role_agent_b:critic"},
            },
        },
        {"role_agent_a", "role_agent_b"},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "explicit role subagent orchestration should succeed");
    Expect(result.steps.size() == 2, "explicit role subagent orchestration should record two steps");
    Expect(result.steps[0].summary.find("[planner] split deterministic work") != std::string::npos,
        "explicit role mapping should apply planner role to first agent");
    Expect(result.steps[1].summary.find("[critic] split deterministic work") != std::string::npos,
        "explicit role mapping should apply critic role to second agent");
    Expect(result.output_json.find("\"roles\":\"planner,critic\"") != std::string::npos,
        "explicit role subagent output should include role assignment summary");
}

void TestSubagentManagerUsesSubtaskObjectives(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_subtasks_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("subtask_agent_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("subtask_agent_b", "review"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-subtasks",
            .task_type = "analysis",
            .objective = "coordinate decomposed work",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"roles", "subtask_agent_a:planner,subtask_agent_b:critic"},
                {"subtasks", "planner=Draft the implementation plan;critic=Review the risk list"},
            },
        },
        {"subtask_agent_a", "subtask_agent_b"},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "subtask objective orchestration should succeed");
    Expect(result.steps.size() == 2, "subtask objective orchestration should record two steps");
    Expect(result.steps[0].summary.find("[planner] Draft the implementation plan") != std::string::npos,
        "subtask list should route planner-specific objective");
    Expect(result.steps[1].summary.find("[critic] Review the risk list") != std::string::npos,
        "subtask list should route critic-specific objective");
}

void TestSubagentManagerAutoDecomposesWithPlanner(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("auto_worker_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("auto_worker_b", "review"));
    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose",
            .task_type = "analysis",
            .objective = "Coordinate a complex implementation",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"roles", "auto_worker_a:planner,auto_worker_b:critic"},
                {"auto_decompose", "true"},
                {"decomposition_agent", "local_planner"},
            },
        },
        {"auto_worker_a", "auto_worker_b"},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "auto decomposition orchestration should succeed");
    Expect(result.output_json.find(R"("decomposition_agent":"local_planner")") != std::string::npos,
        "auto decomposition output should identify the decomposition agent");
    Expect(result.steps.size() == 2, "auto decomposition should still run requested workers");
    Expect(result.steps[0].summary.find("Clarify the requested outcome") != std::string::npos,
        "auto decomposition should route first generated plan action to first worker role");
    Expect(result.steps[1].summary.find("Inspect relevant workspace state") != std::string::npos,
        "auto decomposition should route second generated plan action to second worker role");
}

void TestSubagentManagerRejectsInvalidDecompositionOutput(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_invalid_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("invalid_worker", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "invalid_decomposer",
        true,
        ContentJson("missing plan actions")));
    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-invalid",
            .task_type = "analysis",
            .objective = "Coordinate invalid decomposition",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"auto_decompose", "true"},
                {"decomposition_agent", "invalid_decomposer"},
            },
        },
        {"invalid_worker"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "auto decomposition should reject planner output without plan actions");
    Expect(result.error_code == "DecompositionOutputInvalid",
        "invalid decomposition output should return a clear error code");
    Expect(result.steps.empty(), "invalid decomposition output should fail before starting workers");
}

void TestSubagentManagerAutoDecomposeAcceptsRootPlanSteps(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_root_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("root_worker_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("root_worker_b", "review"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "root_decomposer",
        true,
        R"({"plan_steps":[{"action":"step1"},{"action":"step2"}]})"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-root",
            .task_type = "analysis",
            .objective = "coordinate strict root plan_steps",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"roles", "root_worker_a:planner,root_worker_b:critic"},
                {"auto_decompose", "true"},
                {"decomposition_agent", "root_decomposer"},
            },
        },
        {"root_worker_a", "root_worker_b"},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "strict root plan_steps should drive auto decomposition");
    Expect(result.steps.size() == 2, "strict root plan_steps should inject two subtasks");
    Expect(result.steps[0].summary.find("step1") != std::string::npos,
        "first injected subtask should carry root plan_steps[0].action");
    Expect(result.steps[1].summary.find("step2") != std::string::npos,
        "second injected subtask should carry root plan_steps[1].action");
}

// Each of these proves the substring-search injection vector is closed: the
// planner output contains the literal text "action" but never as a typed
// plan_steps[*].action field.
void TestSubagentManagerAutoDecomposeRejectsActionSubstringInjection(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_inject_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("inject_worker", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "inject_decomposer",
        true,
        R"({"summary":"action: do evil"})"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-inject",
            .task_type = "analysis",
            .objective = "reject substring action injection",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"auto_decompose", "true"},
                {"decomposition_agent", "inject_decomposer"},
            },
        },
        {"inject_worker"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "summary text containing 'action:' must not become a subtask");
    Expect(result.error_code == "DecompositionOutputInvalid",
        "substring injection attempts should surface as DecompositionOutputInvalid");
    Expect(result.steps.empty(), "substring injection attempts should never start workers");
}

void TestSubagentManagerAutoDecomposeRejectsNonArrayPlanSteps(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_nonarray_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("nonarray_worker", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "nonarray_decomposer",
        true,
        R"({"plan_steps":"not an array"})"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-nonarray",
            .task_type = "analysis",
            .objective = "reject non-array plan_steps",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"auto_decompose", "true"},
                {"decomposition_agent", "nonarray_decomposer"},
            },
        },
        {"nonarray_worker"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "plan_steps must be an array");
    Expect(result.error_code == "DecompositionOutputInvalid",
        "non-array plan_steps should surface as DecompositionOutputInvalid");
    Expect(result.steps.empty(), "non-array plan_steps should never start workers");
}

void TestSubagentManagerAutoDecomposeRejectsMissingActionField(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_missing_action_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("missing_action_worker", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "missing_action_decomposer",
        true,
        R"({"plan_steps":[{"foo":"x"}]})"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-missing-action",
            .task_type = "analysis",
            .objective = "reject plan_steps without action",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"auto_decompose", "true"},
                {"decomposition_agent", "missing_action_decomposer"},
            },
        },
        {"missing_action_worker"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "plan_steps elements must contain a string action");
    Expect(result.error_code == "DecompositionOutputInvalid",
        "missing action key should surface as DecompositionOutputInvalid");
    Expect(result.steps.empty(), "missing action key should never start workers");
}

void TestSubagentManagerAutoDecomposeRejectsInvalidJson(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_invalid_json_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("invalid_json_worker", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "invalid_json_decomposer",
        true,
        "this is not json {"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-invalid-json",
            .task_type = "analysis",
            .objective = "reject invalid planner json",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"auto_decompose", "true"},
                {"decomposition_agent", "invalid_json_decomposer"},
            },
        },
        {"invalid_json_worker"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "planner output must be parseable JSON");
    Expect(result.error_code == "DecompositionOutputInvalid",
        "JSON parse errors should surface as DecompositionOutputInvalid");
    Expect(result.steps.empty(), "JSON parse errors should never start workers");
}

void TestSubagentManagerPropagatesDecompositionFailure(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_decompose_failed_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("failed_worker", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<DecompositionTestAgent>(
        "failed_decomposer",
        false,
        ContentJson("failed")));
    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-decompose-failed",
            .task_type = "analysis",
            .objective = "Coordinate failing decomposition",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"auto_decompose", "true"},
                {"decomposition_agent", "failed_decomposer"},
            },
        },
        {"failed_worker"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "auto decomposition should fail when the planner fails");
    Expect(result.error_code == "PlannerFailed", "decomposition failure should preserve planner error code");
    Expect(result.error_message == "configured planner failure",
        "decomposition failure should preserve planner error message");
    Expect(result.steps.empty(), "failed decomposition should not start workers");
}

void TestSubagentManagerParallelRun(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("parallel_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("parallel_b", "review"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-parallel",
            .task_type = "analysis",
            .objective = "coordinate two test agents in parallel",
            .workspace_path = workspace,
        },
        {"parallel_a", "parallel_b"},
        agentos::SubagentExecutionMode::parallel);

    Expect(result.success, "parallel subagent orchestration should succeed");
    Expect(result.steps.size() == 2, "parallel subagent orchestration should record two steps");
    Expect(result.output_json.find("success_count") != std::string::npos, "parallel subagent output should summarize success count");
}

void TestSubagentManagerParallelConcurrencyLimit(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_parallel_limit_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("limited_parallel_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("limited_parallel_b", "analysis"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager,
        4,
        1);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-parallel-limit",
            .task_type = "analysis",
            .objective = "reject too many parallel subagents",
            .workspace_path = isolated_workspace,
        },
        {"limited_parallel_a", "limited_parallel_b"},
        agentos::SubagentExecutionMode::parallel);

    Expect(!result.success, "parallel subagent orchestration should enforce max_parallel_subagents");
    Expect(result.error_code == "TooManyParallelSubagents", "parallel limit should return a clear error code");
    Expect(result.steps.empty(), "parallel limit should reject before starting subagent work");
}

void TestSubagentManagerCostLimit(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_cost_limit_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("cost_agent_a", "analysis", 0.40));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("cost_agent_b", "analysis", 0.35));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager,
        4,
        4);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-cost-limit",
            .task_type = "analysis",
            .objective = "enforce estimated subagent budget",
            .workspace_path = isolated_workspace,
            .budget_limit = 0.50,
        },
        {"cost_agent_a", "cost_agent_b"},
        agentos::SubagentExecutionMode::parallel);

    Expect(!result.success, "subagent orchestration should fail when estimated cost exceeds budget_limit");
    Expect(result.error_code == "SubagentCostLimitExceeded", "cost limit should return a clear error code");
    Expect(result.steps.size() == 2, "cost limit should report completed subagent steps");
    Expect(result.output_json.find("\"estimated_cost\":0.75") != std::string::npos, "subagent output should include estimated total cost");
    const auto stats = runtime.memory_manager.agent_stats();
    Expect(stats.contains("cost_agent_a") &&
        stats.at("cost_agent_a").avg_cost > 0.39 &&
        stats.at("cost_agent_a").avg_cost < 0.41,
        "memory stats should record agent estimated cost");
}

void TestSubagentManagerAutoSelectsCandidates(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_select_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("auto_analysis_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("auto_review_b", "review"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-select",
            .task_type = "analysis",
            .objective = "auto-select matching subagent candidates",
            .workspace_path = isolated_workspace,
        },
        {},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "automatic subagent selection should succeed when a matching healthy agent exists");
    Expect(result.steps.size() == 1, "automatic subagent selection should prefer capability-matching candidates");
    Expect(result.steps.front().target_name == "auto_analysis_a", "automatic subagent selection should choose matching capability");
    Expect(result.output_json.find("auto_analysis_a") != std::string::npos, "automatic subagent output should include selected agent");
}

void TestSubagentManagerAutoSelectionUsesLessons(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_auto_lesson_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("auto_lesson_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("auto_lesson_b", "analysis"));
    runtime.memory_manager.lesson_store().save(agentos::LessonRecord{
        .lesson_id = "analysis|auto_lesson_a|AgentFailed",
        .task_type = "analysis",
        .target_name = "auto_lesson_a",
        .error_code = "AgentFailed",
        .summary = "auto-selected agent failed repeatedly",
        .occurrence_count = 2,
        .last_task_id = "previous-auto-subagent-failure",
        .enabled = true,
    });

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-auto-lesson",
            .task_type = "analysis",
            .objective = "auto-select with lesson penalty",
            .workspace_path = isolated_workspace,
        },
        {},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "automatic subagent selection should still succeed with lesson penalties");
    Expect(result.steps.size() == 2, "automatic subagent selection should keep all matching candidates within limit");
    Expect(result.steps.front().target_name == "auto_lesson_b", "automatic subagent selection should rank lesson-penalized agents later");
    Expect(result.steps[1].target_name == "auto_lesson_a", "lesson-penalized candidate should remain available after healthier candidate");
}

void TestSubagentManagerPolicyDeniesRemoteWithoutPairing(const std::filesystem::path& workspace) {
    TrustedTestRuntime runtime(workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("remote_agent", "analysis"));

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-remote-denied",
            .task_type = "analysis",
            .objective = "remote subagent orchestration without pairing",
            .workspace_path = workspace,
            .remote_trigger = true,
            .origin_identity_id = "unpaired-phone",
            .origin_device_id = "unpaired-device",
        },
        {"remote_agent"},
        agentos::SubagentExecutionMode::sequential);

    Expect(!result.success, "remote subagent orchestration should be denied without pairing");
    Expect(result.steps.size() == 1, "remote denied subagent orchestration should record denied step");
    Expect(result.steps.front().error_code == "PolicyDenied", "remote denied subagent orchestration should fail at policy layer");
}

void TestWorkspaceSessionRunsAgentInSession(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "workspace_session_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::AgentRegistry agent_registry;
    agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());

    agentos::WorkspaceSession session(agent_registry, isolated_workspace, "workspace-session-smoke");
    Expect(session.open_agent("local_planner"), "workspace session should open a session-capable agent");

    const auto opened = session.find("local_planner");
    Expect(opened.has_value(), "workspace session should expose opened agent session");
    if (opened.has_value()) {
        Expect(opened->workspace_session_id == "workspace-session-smoke", "workspace session should preserve workspace session id");
        Expect(opened->agent_session_id.find("local-session-") == 0, "workspace session should preserve provider session id");
        Expect(opened->active, "workspace session should mark opened session active");
    }

    const auto result = session.run_agent_task("local_planner", agentos::AgentTask{
        .task_id = "workspace-session-task",
        .task_type = "analysis",
        .objective = "run through workspace session",
    });
    Expect(result.success, "workspace session should run an agent task inside the opened session");
    Expect(result.summary.find("[local-session-") != std::string::npos, "workspace session should use run_task_in_session");

    Expect(session.close_agent("local_planner"), "workspace session should close an active agent session");
    const auto closed = session.find("local_planner");
    Expect(closed.has_value() && !closed->active, "workspace session should mark closed session inactive");

    const auto after_close = session.run_agent_task("local_planner", agentos::AgentTask{
        .task_id = "workspace-session-after-close",
        .task_type = "analysis",
        .objective = "run after close",
    });
    Expect(!after_close.success, "workspace session should reject task after session close");
    Expect(after_close.error_code == "WorkspaceSessionNotOpen", "closed workspace session should return a clear error");
}

void TestWorkspaceSessionRejectsUnsupportedAgent(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "workspace_session_unsupported_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::AgentRegistry agent_registry;
    agent_registry.register_agent(std::make_shared<StaticTestAgent>("stateless_agent", "analysis"));

    agentos::WorkspaceSession session(agent_registry, isolated_workspace, "workspace-session-unsupported");
    Expect(!session.open_agent("stateless_agent"), "workspace session should reject agents that do not support sessions");
    Expect(session.sessions().empty(), "workspace session should not store unsupported agent sessions");

    const auto result = session.run_agent_task("stateless_agent", agentos::AgentTask{
        .task_id = "workspace-session-unsupported-task",
        .task_type = "analysis",
        .objective = "run unsupported session",
    });
    Expect(!result.success, "workspace session should not run without an opened session");
    Expect(result.error_code == "WorkspaceSessionNotOpen", "unsupported workspace session run should return a clear error");
}

void TestLocalPlanningAgentV2Smoke(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "local_planner_v2_smoke_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::LocalPlanningAgent agent;
    auto cancel = std::make_shared<agentos::CancellationToken>();
    std::vector<agentos::AgentEvent> events;
    auto on_event = [&](const agentos::AgentEvent& event) {
        events.push_back(event);
        return true;
    };

    const agentos::AgentInvocation invocation{
        .task_id = "v2-smoke",
        .objective = "exercise the V2 invoke path",
        .workspace_path = isolated_workspace,
        .context = {{"task_type", "analysis"}},
        .cancel = cancel,
    };

    const auto result = agent.invoke(invocation, on_event);

    Expect(result.success, "V2 invoke should succeed for an offline planner");
    Expect(!result.summary.empty(), "V2 invoke should return a non-empty summary");
    Expect(result.usage.cost_usd == 0.0, "local planner V2 invoke should report zero cost");

    bool saw_status = false;
    bool saw_usage = false;
    bool saw_final = false;
    for (const auto& event : events) {
        if (event.kind == agentos::AgentEvent::Kind::Status) {
            saw_status = true;
        } else if (event.kind == agentos::AgentEvent::Kind::Usage) {
            saw_usage = true;
        } else if (event.kind == agentos::AgentEvent::Kind::Final) {
            saw_final = true;
        }
    }
    Expect(saw_status, "V2 invoke should emit at least one Status event");
    Expect(saw_usage, "V2 invoke should emit a Usage event");
    Expect(saw_final, "V2 invoke should emit a Final event before returning");
}

void TestLocalPlanningAgentV2Cancels(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "local_planner_v2_cancel_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::LocalPlanningAgent agent;
    auto cancel = std::make_shared<agentos::CancellationToken>();
    // Pre-cancel: the deterministic offline planner finishes too fast for a
    // racing thread to win reliably, so we assert the cancel-before-invoke
    // path returns the documented "Cancelled" code.
    cancel->cancel();

    const agentos::AgentInvocation invocation{
        .task_id = "v2-cancel",
        .objective = "ensure cancellation is honored",
        .workspace_path = isolated_workspace,
        .context = {{"task_type", "analysis"}},
        .cancel = cancel,
    };

    const auto started = std::chrono::steady_clock::now();
    const auto result = agent.invoke(invocation);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    Expect(!result.success, "cancelled V2 invoke should not report success");
    Expect(result.error_code == "Cancelled", "cancelled V2 invoke should set the documented Cancelled error code");
    Expect(elapsed.count() < 1000, "cancelled V2 invoke should return promptly");
}

// V2-aware fake adapter: implements both interfaces so we can prove that
// SubagentManager::run_one prefers invoke() over run_task() when both are
// available, and that AgentUsage.cost_usd flows through to step cost.
class V2DualPathAgent final : public agentos::IAgentAdapter, public agentos::IAgentAdapterV2 {
public:
    V2DualPathAgent(std::string name, std::string capability)
        : name_(std::move(name)), capability_(std::move(capability)) {}

    agentos::AgentProfile profile() const override {
        return {
            .agent_name = name_,
            .version = "test",
            .description = "V2-aware dual-path test agent",
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
        last_auth_profile_ = task.auth_profile;
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

    agentos::AgentResult invoke(const agentos::AgentInvocation& invocation,
                                const agentos::AgentEventCallback& /*on_event*/) override {
        v2_calls_ += 1;
        last_context_ = invocation.context;
        last_auth_profile_ = invocation.auth_profile;
        agentos::AgentResult result;
        result.success = true;
        result.summary = "v2 " + invocation.objective;
        result.duration_ms = 2;
        // V2 path reports measured usage; orchestrator should prefer this over
        // legacy estimated_cost (0.99 vs 0.42).
        result.usage.cost_usd = 0.42;
        result.usage.turns = 1;
        return result;
    }

    int legacy_calls() const { return legacy_calls_; }
    int v2_calls() const { return v2_calls_; }
    const agentos::StringMap& last_context() const { return last_context_; }
    const std::optional<std::string>& last_auth_profile() const { return last_auth_profile_; }

private:
    std::string name_;
    std::string capability_;
    int legacy_calls_ = 0;
    int v2_calls_ = 0;
    agentos::StringMap last_context_;
    std::optional<std::string> last_auth_profile_;
};

void TestSubagentManagerPrefersV2InvokeWhenAvailable(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_v2_routing";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    auto v2_agent = std::make_shared<V2DualPathAgent>("agent_v2", "analysis");
    runtime.agent_registry.register_agent(v2_agent);

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-v2-routing",
            .task_type = "analysis",
            .objective = "exercise V2 routing",
            .workspace_path = isolated_workspace,
            .auth_profile = "team-profile",
        },
        {"agent_v2"},
        agentos::SubagentExecutionMode::sequential);

    Expect(result.success, "V2-routed subagent run should succeed");
    Expect(v2_agent->v2_calls() == 1,
        "SubagentManager should call invoke() exactly once on a V2-capable adapter");
    Expect(v2_agent->legacy_calls() == 0,
        "SubagentManager should NOT fall back to run_task() when invoke() succeeds");
    Expect(result.steps.size() == 1, "V2-routed run should record one step");
    if (result.steps.size() == 1) {
        const auto& step = result.steps[0];
        Expect(step.summary.find("v2 ") != std::string::npos,
            "step summary should reflect the V2 invoke() output, not the legacy run_task()");
        // 0.42 (V2 usage.cost_usd) wins over 0.99 (legacy estimated_cost).
        Expect(step.estimated_cost > 0.41 && step.estimated_cost < 0.43,
            "step.estimated_cost should be sourced from AgentResult.usage.cost_usd on the V2 path");
    }
    // Spot-check that structured invocation context made it through the
    // TaskRequest -> AgentInvocation translation in run_one.
    const auto& ctx = v2_agent->last_context();
    Expect(ctx.count("task_type") == 1 && ctx.at("task_type") == "analysis",
        "V2 invocation context should carry the original task_type");
    Expect(ctx.count("role") == 1 && !ctx.at("role").empty(),
        "V2 invocation context should carry the assigned role");
    Expect(ctx.count("parent_task_id") == 1 && ctx.at("parent_task_id") == "subagent-v2-routing",
        "V2 invocation context should carry the parent task_id");
    Expect(v2_agent->last_auth_profile().has_value() && *v2_agent->last_auth_profile() == "team-profile",
        "SubagentManager should forward TaskRequest.auth_profile into V2 AgentInvocation");
}

void TestSubagentManagerCancellationShortCircuitsSequential(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_cancel_sequential";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    auto agent_a = std::make_shared<V2DualPathAgent>("agent_seq_a", "analysis");
    auto agent_b = std::make_shared<V2DualPathAgent>("agent_seq_b", "analysis");
    runtime.agent_registry.register_agent(agent_a);
    runtime.agent_registry.register_agent(agent_b);

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    auto cancel = std::make_shared<agentos::CancellationToken>();
    cancel->cancel();

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-cancel-seq",
            .task_type = "analysis",
            .objective = "should not dispatch any agent",
            .workspace_path = isolated_workspace,
        },
        {"agent_seq_a", "agent_seq_b"},
        agentos::SubagentExecutionMode::sequential,
        cancel);

    Expect(!result.success, "pre-tripped cancel should fail the sequential subagent run");
    Expect(result.steps.size() == 2,
        "sequential cancel should still record one step per requested agent");
    for (const auto& step : result.steps) {
        Expect(step.error_code == "Cancelled",
            "every sequential step under a tripped cancel should report Cancelled");
        Expect(!step.success, "Cancelled steps must not be marked successful");
    }
    Expect(agent_a->v2_calls() == 0 && agent_a->legacy_calls() == 0,
        "first sequential agent should never have been dispatched under a tripped cancel");
    Expect(agent_b->v2_calls() == 0 && agent_b->legacy_calls() == 0,
        "second sequential agent should never have been dispatched under a tripped cancel");
}

void TestSubagentManagerCancellationShortCircuitsParallel(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "subagent_cancel_parallel";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    auto agent_a = std::make_shared<V2DualPathAgent>("agent_par_a", "analysis");
    auto agent_b = std::make_shared<V2DualPathAgent>("agent_par_b", "analysis");
    runtime.agent_registry.register_agent(agent_a);
    runtime.agent_registry.register_agent(agent_b);

    agentos::SubagentManager manager(
        runtime.agent_registry,
        runtime.policy_engine,
        runtime.audit_logger,
        runtime.memory_manager);

    auto cancel = std::make_shared<agentos::CancellationToken>();
    cancel->cancel();

    const auto result = manager.run(
        agentos::TaskRequest{
            .task_id = "subagent-cancel-par",
            .task_type = "analysis",
            .objective = "should cancel both parallel branches",
            .workspace_path = isolated_workspace,
        },
        {"agent_par_a", "agent_par_b"},
        agentos::SubagentExecutionMode::parallel,
        cancel);

    Expect(!result.success, "pre-tripped cancel should fail the parallel subagent run");
    Expect(result.steps.size() == 2,
        "parallel cancel should still produce one step per requested agent");
    for (const auto& step : result.steps) {
        Expect(step.error_code == "Cancelled",
            "every parallel step under a tripped cancel should report Cancelled");
    }
    // Pre-tripped tokens hit the run_one prologue check before the V2 invoke
    // dispatch, so neither path on either adapter should have been called.
    Expect(agent_a->v2_calls() == 0 && agent_a->legacy_calls() == 0,
        "parallel agent_a should not have been dispatched under a pre-tripped cancel");
    Expect(agent_b->v2_calls() == 0 && agent_b->legacy_calls() == 0,
        "parallel agent_b should not have been dispatched under a pre-tripped cancel");
}

void TestAgentLoopRoutesThroughV2InvokeAndForwardsUsageCost(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "agent_loop_v2_routing";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    auto agent = std::make_shared<V2DualPathAgent>("loop_agent_v2", "analysis");
    runtime.agent_registry.register_agent(agent);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "agent-loop-v2-routing",
        .task_type = "analysis",
        .objective = "exercise V2 routing through AgentLoop",
        .workspace_path = isolated_workspace,
        .auth_profile = "loop-profile",
    });

    Expect(result.success, "AgentLoop V2 routing should succeed");
    Expect(agent->v2_calls() == 1,
        "AgentLoop should call invoke() exactly once on a V2-capable agent");
    Expect(agent->legacy_calls() == 0,
        "AgentLoop should NOT fall back to run_task() when invoke() succeeds");
    Expect(result.steps.size() == 1, "AgentLoop V2 routing should record one step");
    if (result.steps.size() == 1) {
        Expect(result.steps[0].summary.find("v2 ") != std::string::npos,
            "AgentLoop step summary should reflect the V2 invoke() output");
        Expect(result.steps[0].estimated_cost > 0.41 && result.steps[0].estimated_cost < 0.43,
            "AgentLoop step.estimated_cost should source from AgentResult.usage.cost_usd on the V2 path");
    }
    Expect(agent->last_auth_profile().has_value() && *agent->last_auth_profile() == "loop-profile",
        "AgentLoop should forward TaskRequest.auth_profile into V2 AgentInvocation");
}

void TestAgentLoopHonorsCancellationBeforeAgentDispatch(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "agent_loop_cancel_dispatch";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    auto agent = std::make_shared<V2DualPathAgent>("loop_agent_cancel", "analysis");
    runtime.agent_registry.register_agent(agent);

    auto cancel = std::make_shared<agentos::CancellationToken>();
    cancel->cancel();

    const auto result = runtime.loop.run(
        agentos::TaskRequest{
            .task_id = "agent-loop-cancel",
            .task_type = "analysis",
            .objective = "should not dispatch under tripped cancel",
            .workspace_path = isolated_workspace,
        },
        cancel);

    Expect(!result.success, "AgentLoop should fail when the cancel token is pre-tripped");
    Expect(result.error_code == "Cancelled",
        "AgentLoop should set error_code=Cancelled for pre-tripped cancellations");
    // Pre-routing cancel returns before dispatch; agent should not have been
    // called by either entry point.
    Expect(agent->v2_calls() == 0,
        "AgentLoop pre-tripped cancel should not invoke V2 path");
    Expect(agent->legacy_calls() == 0,
        "AgentLoop pre-tripped cancel should not invoke legacy path");
}

void TestSignalCancellationIsIdempotent() {
    auto first = agentos::InstallSignalCancellation();
    auto second = agentos::InstallSignalCancellation();
    Expect(first.get() == second.get(),
        "InstallSignalCancellation should be idempotent — same token across calls");
    Expect(first != nullptr,
        "InstallSignalCancellation should always return a non-null token");
    // The token is global state shared across the whole process. We cannot
    // safely cancel() it here — that would poison every subsequent test in
    // this binary. Just assert observability.
    Expect(!first->is_cancelled(),
        "freshly installed signal cancellation token should not be cancelled");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestSubagentManagerSequentialRun(workspace);
    TestSubagentManagerUsesExplicitRoles(workspace);
    TestSubagentManagerUsesSubtaskObjectives(workspace);
    TestSubagentManagerAutoDecomposesWithPlanner(workspace);
    TestSubagentManagerRejectsInvalidDecompositionOutput(workspace);
    TestSubagentManagerAutoDecomposeAcceptsRootPlanSteps(workspace);
    TestSubagentManagerAutoDecomposeRejectsActionSubstringInjection(workspace);
    TestSubagentManagerAutoDecomposeRejectsNonArrayPlanSteps(workspace);
    TestSubagentManagerAutoDecomposeRejectsMissingActionField(workspace);
    TestSubagentManagerAutoDecomposeRejectsInvalidJson(workspace);
    TestSubagentManagerPropagatesDecompositionFailure(workspace);
    TestSubagentManagerParallelRun(workspace);
    TestSubagentManagerParallelConcurrencyLimit(workspace);
    TestSubagentManagerCostLimit(workspace);
    TestSubagentManagerAutoSelectsCandidates(workspace);
    TestSubagentManagerAutoSelectionUsesLessons(workspace);
    TestSubagentManagerPolicyDeniesRemoteWithoutPairing(workspace);
    TestWorkspaceSessionRunsAgentInSession(workspace);
    TestWorkspaceSessionRejectsUnsupportedAgent(workspace);
    TestLocalPlanningAgentV2Smoke(workspace);
    TestLocalPlanningAgentV2Cancels(workspace);
    TestSubagentManagerPrefersV2InvokeWhenAvailable(workspace);
    TestSubagentManagerCancellationShortCircuitsSequential(workspace);
    TestSubagentManagerCancellationShortCircuitsParallel(workspace);
    TestAgentLoopRoutesThroughV2InvokeAndForwardsUsageCost(workspace);
    TestAgentLoopHonorsCancellationBeforeAgentDispatch(workspace);
    TestSignalCancellationIsIdempotent();

    if (failures != 0) {
        std::cerr << failures << " subagent/session test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_subagent_session_tests passed\n";
    return 0;
}
