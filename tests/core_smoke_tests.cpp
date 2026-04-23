#include "auth/auth_manager.hpp"
#include "auth/credential_broker.hpp"
#include "auth/provider_adapters.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/orchestration/subagent_manager.hpp"
#include "core/policy/permission_model.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/mock_planning_agent.hpp"
#include "hosts/cli/cli_host.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"
#include "trust/allowlist_store.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_manager.hpp"
#include "trust/trust_policy.hpp"

#include <filesystem>
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void SetEnvForTest(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
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
          policy_engine(trust_policy),
          execution_cache(workspace / "trusted_execution_cache.tsv"),
          audit_logger(workspace / "trusted_audit.log"),
          memory_manager(workspace / "trusted_memory"),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

class StaticTestAgent final : public agentos::IAgentAdapter {
public:
    StaticTestAgent(std::string name, std::string capability)
        : name_(std::move(name)), capability_(std::move(capability)) {}

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
            .structured_output_json = "{}",
            .duration_ms = 1,
            .estimated_cost = 0.0,
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
};

agentos::CliSpec MakeEnvironmentProbeSpec(std::vector<std::string> env_allowlist = {}) {
#ifdef _WIN32
    return {
        .name = "env_probe",
        .description = "Probe whether a test environment variable reaches a child process.",
        .binary = "cmd",
        .args_template = {"/d", "/s", "/c", "if defined AGENTOS_CLI_LEAK_TEST (echo present=%AGENTOS_CLI_LEAK_TEST%) else echo missing"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 3000,
        .output_limit_bytes = 4096,
        .env_allowlist = std::move(env_allowlist),
    };
#else
    return {
        .name = "env_probe",
        .description = "Probe whether a test environment variable reaches a child process.",
        .binary = "sh",
        .args_template = {"-c", "if [ -n \"$AGENTOS_CLI_LEAK_TEST\" ]; then echo present=$AGENTOS_CLI_LEAK_TEST; else echo missing; fi"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 3000,
        .output_limit_bytes = 4096,
        .env_allowlist = std::move(env_allowlist),
    };
#endif
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_smoke_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(runtime.skill_registry));
    runtime.agent_registry.register_agent(std::make_shared<agentos::MockPlanningAgent>());
}

void RegisterCore(TrustedTestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(runtime.skill_registry));
    runtime.agent_registry.register_agent(std::make_shared<agentos::MockPlanningAgent>());
}

void TestFileSkillLoop(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto write_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "write",
        .task_type = "write_file",
        .objective = "write a smoke test file",
        .workspace_path = workspace,
        .inputs = {
            {"path", "notes/test.txt"},
            {"content", "alpha"},
        },
    });

    Expect(write_result.success, "file_write should succeed");
    Expect(std::filesystem::exists(workspace / "notes" / "test.txt"), "file_write should create the file");

    const auto read_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "read",
        .task_type = "read_file",
        .objective = "read a smoke test file",
        .workspace_path = workspace,
        .inputs = {
            {"path", "notes/test.txt"},
        },
    });

    Expect(read_result.success, "file_read should succeed");
    Expect(read_result.output_json.find("alpha") != std::string::npos, "file_read output should contain written content");
    Expect(runtime.memory_manager.task_log().size() == 2, "memory manager should record executed tasks");
}

void TestPolicyDeniesWorkspaceEscape(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "escape",
        .task_type = "read_file",
        .objective = "try to escape workspace",
        .workspace_path = workspace,
        .inputs = {
            {"path", "../outside.txt"},
        },
    });

    Expect(!result.success, "workspace escape should be denied");
    Expect(result.error_code == "PolicyDenied", "workspace escape should fail at policy layer");
}

void TestPermissionModelDeniesUnknownPermission(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto decision = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-permission",
            .task_type = "custom",
            .objective = "validate unknown permission handling",
            .workspace_path = workspace,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "low",
            .permissions = {"filesystem.read", "unknown.permission"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "README.md"},
            },
        });

    Expect(!decision.allowed, "unknown skill permission should be denied");
    Expect(decision.reason.find("unknown.permission") != std::string::npos, "unknown permission denial should name the permission");
}

void TestPermissionModelNamespaceWildcard(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto decision = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "wildcard-permission",
            .task_type = "custom",
            .objective = "validate namespace wildcard handling",
            .workspace_path = workspace,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "low",
            .permissions = {"filesystem.*"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "../outside.txt"},
            },
        });

    Expect(!decision.allowed, "filesystem.* permission should be treated as filesystem access");
    Expect(decision.reason == "path escapes the active workspace", "filesystem.* should trigger workspace boundary checks");
    Expect(agentos::PermissionModel::has_permission({"filesystem.*"}, agentos::PermissionNames::FilesystemWrite),
        "permission model should match namespace wildcard permissions");
}

void TestPermissionModelUnknownRiskRequiresApproval(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto denied = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-risk-denied",
            .task_type = "custom",
            .objective = "validate unknown risk handling",
            .workspace_path = workspace,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "experimental",
            .permissions = {},
        },
        agentos::SkillCall{});

    Expect(!denied.allowed, "unknown risk level should require high-risk approval");

    const auto allowed = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-risk-allowed",
            .task_type = "custom",
            .objective = "validate unknown risk override",
            .workspace_path = workspace,
            .allow_high_risk = true,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "experimental",
            .permissions = {},
        },
        agentos::SkillCall{});

    Expect(allowed.allowed, "allow_high_risk should override unknown risk level");
}

void TestRemoteTaskRequiresPairing(const std::filesystem::path& workspace) {
    TrustedTestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto denied = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-denied",
        .task_type = "read_file",
        .objective = "remote read without pairing",
        .workspace_path = workspace,
        .remote_trigger = true,
        .origin_identity_id = "phone",
        .origin_device_id = "device-1",
        .inputs = {
            {"path", "missing.txt"},
        },
    });

    Expect(!denied.success, "unpaired remote task should be denied");
    Expect(denied.error_code == "PolicyDenied", "unpaired remote task should fail at policy layer");

    runtime.identity_manager.ensure("phone", "local-user", "test phone");
    runtime.pairing_manager.pair("phone", "device-1", "test phone", {"task.submit"});

    const auto write = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-allowed-write",
        .task_type = "write_file",
        .objective = "create remote-readable file",
        .workspace_path = workspace,
        .inputs = {
            {"path", "remote/allowed.txt"},
            {"content", "ok"},
        },
    });
    Expect(write.success, "local write should prepare remote read file");

    const auto allowed = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-allowed",
        .task_type = "read_file",
        .objective = "remote read after pairing",
        .workspace_path = workspace,
        .remote_trigger = true,
        .origin_identity_id = "phone",
        .origin_device_id = "device-1",
        .inputs = {
            {"path", "remote/allowed.txt"},
        },
    });

    Expect(allowed.success, "paired remote task should be allowed");
    Expect(std::filesystem::exists(workspace / "trust" / "identities.tsv"), "identity store should be persisted");
    Expect(std::filesystem::exists(workspace / "trust" / "allowlist.tsv"), "allowlist should be persisted");
}

void TestIdentityManagerPersistsAndUpdates(const std::filesystem::path& workspace) {
    const auto identity_store = workspace / "trust_identity" / "identities.tsv";

    agentos::IdentityManager manager(identity_store);
    const auto saved = manager.save(agentos::Identity{
        .identity_id = "phone",
        .user_id = "user-a",
        .label = "dev phone",
    });

    Expect(saved.identity_id == "phone", "identity manager should save identity");
    Expect(manager.find("phone").has_value(), "identity manager should find saved identity");

    manager.save(agentos::Identity{
        .identity_id = "phone",
        .user_id = "user-a",
        .label = "renamed phone",
    });

    agentos::IdentityManager reloaded(identity_store);
    const auto reloaded_identity = reloaded.find("phone");
    Expect(reloaded_identity.has_value(), "identity manager should reload persisted identity");
    if (reloaded_identity.has_value()) {
        Expect(reloaded_identity->label == "renamed phone", "identity manager should update existing identity");
    }

    Expect(reloaded.remove("phone"), "identity manager should remove identity");
    Expect(!reloaded.find("phone").has_value(), "identity manager should not find removed identity");
}

void TestWorkflowRun(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow",
        .task_type = "workflow_run",
        .objective = "run write/patch/read workflow",
        .workspace_path = workspace,
        .inputs = {
            {"workflow", "write_patch_read"},
            {"path", "workflow/result.txt"},
            {"content", "hello"},
            {"find", "hello"},
            {"replace", "done"},
        },
    });

    Expect(result.success, "workflow_run should succeed");
    Expect(result.output_json.find("done") != std::string::npos, "workflow final output should contain patched content");
}

void TestDefaultAgentRoute(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "analysis",
        .task_type = "analysis",
        .objective = "plan a small task",
        .workspace_path = workspace,
    });

    Expect(result.success, "analysis task should route to mock planner");
    Expect(result.route_target == "mock_planner", "default healthy agent should be mock_planner");
}

void TestIdempotentExecutionCache(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const agentos::TaskRequest request{
        .task_id = "idempotent-write",
        .task_type = "write_file",
        .objective = "write a file once",
        .workspace_path = workspace,
        .idempotency_key = "smoke-idempotent-write",
        .inputs = {
            {"path", "idempotent/result.txt"},
            {"content", "first"},
        },
    };

    const auto first = runtime.loop.run(request);
    Expect(first.success, "first idempotent write should succeed");
    Expect(!first.from_cache, "first idempotent write should not come from cache");

    auto second_request = request;
    second_request.task_id = "idempotent-write-replay";
    second_request.inputs["content"] = "second";

    const auto second = runtime.loop.run(second_request);
    Expect(second.success, "second idempotent write should succeed");
    Expect(second.from_cache, "second idempotent write should come from cache");

    const auto content = std::filesystem::path(workspace / "idempotent" / "result.txt");
    Expect(std::filesystem::exists(content), "idempotent target file should exist");
}

void TestPersistentTaskAndStepLogs(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "persistent-log",
        .task_type = "write_file",
        .objective = "write a persistent log entry",
        .workspace_path = workspace,
        .inputs = {
            {"path", "logs/result.txt"},
            {"content", "log"},
        },
    });

    Expect(result.success, "persistent log write should succeed");
    Expect(std::filesystem::exists(workspace / "memory" / "task_log.tsv"), "task_log.tsv should be written");
    Expect(std::filesystem::exists(workspace / "memory" / "step_log.tsv"), "step_log.tsv should be written");
}

void TestWorkflowCandidatesAcrossRestart(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "workflow_candidates_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    {
        TestRuntime runtime(isolated_workspace);
        RegisterCore(runtime);

        for (int index = 0; index < 2; ++index) {
            const auto result = runtime.loop.run(agentos::TaskRequest{
                .task_id = "workflow-candidate-" + std::to_string(index),
                .task_type = "write_file",
                .objective = "generate repeated task pattern",
                .workspace_path = isolated_workspace,
                .inputs = {
                    {"path", "workflow_candidates/result_" + std::to_string(index) + ".txt"},
                    {"content", "pattern"},
                },
            });
            Expect(result.success, "repeated write_file pattern should succeed");
        }

        const auto failed_result = runtime.loop.run(agentos::TaskRequest{
            .task_id = "workflow-candidate-failure",
            .task_type = "write_file",
            .objective = "generate a failed task pattern",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "../outside.txt"},
                {"content", "denied"},
            },
        });
        Expect(!failed_result.success, "failed write_file pattern should be recorded for workflow scoring");
    }

    agentos::MemoryManager reloaded_memory(isolated_workspace / "memory");
    const auto workflows = reloaded_memory.workflow_candidates();
    Expect(!workflows.empty(), "workflow candidates should load from persisted task/step logs");
    const auto workflow = std::find_if(workflows.begin(), workflows.end(), [](const agentos::WorkflowCandidate& candidate) {
        return candidate.trigger_task_type == "write_file";
    });
    Expect(workflow != workflows.end(), "write_file workflow candidate should be generated");
    if (workflow != workflows.end()) {
        Expect(workflow->use_count == 3, "workflow scoring should count successful and failed attempts");
        Expect(workflow->success_count == 2, "workflow scoring should count successful attempts");
        Expect(workflow->failure_count == 1, "workflow scoring should count failed attempts");
        Expect(workflow->success_rate > 0.66 && workflow->success_rate < 0.67, "workflow scoring should compute success rate");
        Expect(workflow->score > 0.0, "workflow scoring should produce a positive candidate score for mostly successful workflows");
    }
    Expect(std::filesystem::exists(isolated_workspace / "memory" / "workflow_candidates.tsv"), "workflow_candidates.tsv should be written");
}

void TestSchedulerRunsDueTask(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(workspace / "scheduler" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "write-once",
        .enabled = true,
        .next_run_epoch_ms = agentos::Scheduler::NowEpochMs() - 1000,
        .interval_seconds = 0,
        .max_runs = 1,
        .run_count = 0,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "scheduled one-shot write",
            .workspace_path = workspace,
            .inputs = {
                {"path", "scheduled/once.txt"},
                {"content", "scheduled"},
            },
        },
    });

    const auto records = scheduler.run_due(runtime.loop, agentos::Scheduler::NowEpochMs());
    Expect(records.size() == 1, "scheduler should run one due task");
    Expect(records.front().result.success, "scheduled write_file should succeed");
    Expect(!records.front().rescheduled, "one-shot scheduled task should not be rescheduled");
    Expect(std::filesystem::exists(workspace / "scheduled" / "once.txt"), "scheduled write should create a file");

    const auto stored = scheduler.find("write-once");
    Expect(stored.has_value(), "scheduled task should remain inspectable after execution");
    Expect(!stored->enabled, "one-shot scheduled task should be disabled after execution");
    Expect(stored->run_count == 1, "one-shot scheduled task should increment run_count");

    agentos::Scheduler reloaded(workspace / "scheduler" / "tasks.tsv");
    const auto reloaded_task = reloaded.find("write-once");
    Expect(reloaded_task.has_value(), "scheduled task should reload from persisted store");
    Expect(reloaded_task->run_count == 1, "scheduled task run_count should persist");
}

void TestSchedulerReschedulesIntervalTask(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(workspace / "scheduler_interval" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "write-twice",
        .enabled = true,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 60,
        .max_runs = 2,
        .run_count = 0,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "scheduled recurring write",
            .workspace_path = workspace,
            .inputs = {
                {"path", "scheduled/twice.txt"},
                {"content", "recurring"},
            },
        },
    });

    const auto first = scheduler.run_due(runtime.loop, 1000);
    Expect(first.size() == 1, "recurring scheduled task should run when due");
    Expect(first.front().rescheduled, "recurring scheduled task should reschedule before max_runs");

    const auto after_first = scheduler.find("write-twice");
    Expect(after_first.has_value(), "recurring scheduled task should remain after first run");
    Expect(after_first->enabled, "recurring scheduled task should remain enabled before max_runs");
    Expect(after_first->next_run_epoch_ms == 61000, "recurring scheduled task should advance next_run_epoch_ms");

    const auto second = scheduler.run_due(runtime.loop, 61000);
    Expect(second.size() == 1, "recurring scheduled task should run a second time");
    Expect(!second.front().rescheduled, "recurring scheduled task should stop after max_runs");

    const auto after_second = scheduler.find("write-twice");
    Expect(after_second.has_value(), "recurring scheduled task should remain inspectable after max_runs");
    Expect(!after_second->enabled, "recurring scheduled task should disable after max_runs");
    Expect(after_second->run_count == 2, "recurring scheduled task should persist final run count");
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
    Expect(runtime.memory_manager.agent_stats().contains("agent_a"), "subagent run should update agent_a stats");
    Expect(runtime.memory_manager.agent_stats().contains("agent_b"), "subagent run should update agent_b stats");
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

void TestCliHostEnvironmentAllowlist(const std::filesystem::path& workspace) {
    SetEnvForTest("AGENTOS_CLI_LEAK_TEST", "secret-env-value");

    agentos::CliHost cli_host;
    const auto denied = cli_host.run(agentos::CliRunRequest{
        .spec = MakeEnvironmentProbeSpec(),
        .workspace_path = workspace,
    });

    Expect(denied.success, "CLI env probe without allowlist should execute successfully");
    Expect(denied.stdout_text.find("missing") != std::string::npos, "CLI env probe should not receive non-allowlisted env var");
    Expect(denied.stdout_text.find("secret-env-value") == std::string::npos, "CLI env probe should not leak env var value");

    const auto allowed = cli_host.run(agentos::CliRunRequest{
        .spec = MakeEnvironmentProbeSpec({"AGENTOS_CLI_LEAK_TEST"}),
        .workspace_path = workspace,
    });

    Expect(allowed.success, "CLI env probe with allowlist should execute successfully");
    Expect(allowed.stdout_text.find("present=secret-env-value") != std::string::npos, "CLI env probe should receive allowlisted env var");
}

void TestAuthApiKeySession(const std::filesystem::path& workspace) {
    SetEnvForTest("AGENTOS_TEST_QWEN_KEY", "test-secret");

    agentos::SessionStore session_store(workspace / "auth" / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::AuthManager auth_manager(session_store);

    auth_manager.register_provider(std::make_shared<agentos::QwenAuthProviderAdapter>(session_store, token_store));

    const auto session = auth_manager.login(
        agentos::AuthProviderId::qwen,
        agentos::AuthMode::api_key,
        {
            {"profile", "smoke"},
            {"api_key_env", "AGENTOS_TEST_QWEN_KEY"},
        });

    Expect(session.managed_by_agentos, "api-key session should be marked as AgentOS managed");
    Expect(session.access_token_ref == "env:AGENTOS_TEST_QWEN_KEY", "api-key session should store only env ref");

    const auto status = auth_manager.status(agentos::AuthProviderId::qwen, "smoke");
    Expect(status.authenticated, "api-key status should authenticate when env var is present");
    Expect(credential_broker.get_access_token(agentos::AuthProviderId::qwen, "smoke") == "test-secret", "credential broker should resolve env ref");
}

void TestAuthUnsupportedMode(const std::filesystem::path& workspace) {
    agentos::SessionStore session_store(workspace / "auth_unsupported" / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);

    auth_manager.register_provider(std::make_shared<agentos::QwenAuthProviderAdapter>(session_store, token_store));

    bool failed = false;
    try {
        (void)auth_manager.login(agentos::AuthProviderId::qwen, agentos::AuthMode::browser_oauth, {{"profile", "smoke"}});
    } catch (const std::exception&) {
        failed = true;
    }

    Expect(failed, "qwen browser oauth should be unsupported in MVP");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestFileSkillLoop(workspace);
    TestPolicyDeniesWorkspaceEscape(workspace);
    TestPermissionModelDeniesUnknownPermission(workspace);
    TestPermissionModelNamespaceWildcard(workspace);
    TestPermissionModelUnknownRiskRequiresApproval(workspace);
    TestIdentityManagerPersistsAndUpdates(workspace);
    TestRemoteTaskRequiresPairing(workspace);
    TestWorkflowRun(workspace);
    TestDefaultAgentRoute(workspace);
    TestIdempotentExecutionCache(workspace);
    TestPersistentTaskAndStepLogs(workspace);
    TestWorkflowCandidatesAcrossRestart(workspace);
    TestSchedulerRunsDueTask(workspace);
    TestSchedulerReschedulesIntervalTask(workspace);
    TestSubagentManagerSequentialRun(workspace);
    TestSubagentManagerParallelRun(workspace);
    TestSubagentManagerPolicyDeniesRemoteWithoutPairing(workspace);
    TestCliHostEnvironmentAllowlist(workspace);
    TestAuthApiKeySession(workspace);
    TestAuthUnsupportedMode(workspace);

    if (failures != 0) {
        std::cerr << failures << " smoke test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_smoke_tests passed\n";
    return 0;
}
