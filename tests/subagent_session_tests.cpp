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
#include "utils/json_utils.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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
        return {
            .success = true,
            .summary = name_ + " handled " + task.objective,
            .structured_output_json = agentos::MakeJsonObject({
                {"content", agentos::QuoteJson(name_ + " handled " + task.objective)},
                {"agent", agentos::QuoteJson(name_)},
                {"provider", agentos::QuoteJson("static_test")},
                {"model", agentos::QuoteJson("static-model")},
                {"task_type", agentos::QuoteJson(task.task_type)},
            }),
            .artifacts = {
                agentos::AgentArtifact{
                    .type = "text",
                    .uri = "memory://" + task.task_id,
                    .content = name_ + " artifact",
                    .metadata_json = agentos::MakeJsonObject({{"source", agentos::QuoteJson("static_test")}}),
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
    Expect(result.output_json.find(R"("metrics":{"duration_ms":1,"estimated_cost":0.00})") != std::string::npos,
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
        agentos::MakeJsonObject({
            {"content", agentos::QuoteJson("missing plan actions")},
        })));
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
        agentos::MakeJsonObject({{"content", agentos::QuoteJson("failed")}})));
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

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestSubagentManagerSequentialRun(workspace);
    TestSubagentManagerUsesExplicitRoles(workspace);
    TestSubagentManagerUsesSubtaskObjectives(workspace);
    TestSubagentManagerAutoDecomposesWithPlanner(workspace);
    TestSubagentManagerRejectsInvalidDecompositionOutput(workspace);
    TestSubagentManagerPropagatesDecompositionFailure(workspace);
    TestSubagentManagerParallelRun(workspace);
    TestSubagentManagerParallelConcurrencyLimit(workspace);
    TestSubagentManagerCostLimit(workspace);
    TestSubagentManagerAutoSelectsCandidates(workspace);
    TestSubagentManagerAutoSelectionUsesLessons(workspace);
    TestSubagentManagerPolicyDeniesRemoteWithoutPairing(workspace);
    TestWorkspaceSessionRunsAgentInSession(workspace);
    TestWorkspaceSessionRejectsUnsupportedAgent(workspace);

    if (failures != 0) {
        std::cerr << failures << " subagent/session test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_subagent_session_tests passed\n";
    return 0;
}
