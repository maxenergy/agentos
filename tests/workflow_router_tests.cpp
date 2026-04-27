#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/local_planning_agent.hpp"
#include "memory/memory_manager.hpp"
#include "memory/workflow_store.hpp"
#include "memory/workflow_validation.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_workflow_router_tests";
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
        structured_output["context_json"] = task.context_json;
        structured_output["constraints_json"] = task.constraints_json;
        return {
            .success = true,
            .summary = name_ + " handled " + task.objective,
            .structured_output_json = structured_output.dump(),
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

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void TestWorkflowRun(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow",
        .task_type = "workflow_run",
        .objective = "run write/patch/read workflow",
        .workspace_path = workspace,
        .idempotency_key = "workflow-run-write-patch-read",
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

void TestWorkflowRunStoredDefinition(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "stored_write_read",
        .trigger_task_type = "workflow_run",
        .ordered_steps = {"file_write", "file_read"},
        .source = "test",
        .enabled = true,
    });

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "stored-workflow",
        .task_type = "workflow_run",
        .objective = "run stored write/read workflow",
        .workspace_path = workspace,
        .idempotency_key = "stored-workflow-run",
        .inputs = {
            {"workflow", "stored_write_read"},
            {"path", "workflow/stored_result.txt"},
            {"content", "stored"},
        },
    });

    Expect(result.success, "workflow_run should execute stored workflow definition");
    Expect(result.output_json.find("stored_write_read") != std::string::npos, "stored workflow output should include workflow name");
    Expect(result.output_json.find("file_write,file_read") != std::string::npos, "stored workflow output should include ordered steps");
    Expect(result.output_json.find("stored") != std::string::npos, "stored workflow output should include final read content");
}

void TestRouterPrefersPromotedWorkflow(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_promoted_workflow_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "auto_write_file_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .source = "promoted_candidate",
        .enabled = true,
        .use_count = 3,
        .success_count = 3,
        .success_rate = 1.0,
        .score = 100.0,
    });

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "router-promoted-workflow",
        .task_type = "write_file",
        .objective = "write through promoted workflow",
        .workspace_path = isolated_workspace,
        .idempotency_key = "router-promoted-workflow",
        .inputs = {
            {"path", "workflow/router_promoted.txt"},
            {"content", "auto-promoted"},
        },
    });

    Expect(result.success, "router should execute promoted workflow for matching task_type");
    Expect(result.route_target == "workflow_run", "router should route matching promoted workflow through workflow_run");
    Expect(result.output_json.find("auto_write_file_workflow") != std::string::npos, "promoted workflow output should include selected workflow name");

    std::ifstream input(isolated_workspace / "workflow" / "router_promoted.txt", std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Expect(buffer.str() == "auto-promoted", "promoted workflow should perform the original write_file operation");
}

void TestRouterWorkflowApplicabilityRequiresInputs(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_applicability_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "requires_find_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content", "find"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto missing_input_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-applicability-skip",
        .task_type = "write_file",
        .objective = "write without optional workflow input",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-applicability-skip",
        .inputs = {
            {"path", "workflow/applicability_skip.txt"},
            {"content", "direct"},
        },
    });

    Expect(missing_input_result.success, "direct write should still succeed when workflow applicability is not satisfied");
    Expect(missing_input_result.route_target == "file_write", "router should skip workflow when required inputs are missing");

    const auto matching_input_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-applicability-match",
        .task_type = "write_file",
        .objective = "write with all workflow inputs",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-applicability-match",
        .inputs = {
            {"path", "workflow/applicability_match.txt"},
            {"content", "workflow"},
            {"find", "unused"},
        },
    });

    Expect(matching_input_result.success, "workflow write should succeed when required inputs are present");
    Expect(matching_input_result.route_target == "workflow_run", "router should select workflow when required inputs are present");
    Expect(matching_input_result.output_json.find("requires_find_write_workflow") != std::string::npos, "selected workflow should be visible in output");
}

void TestRouterWorkflowApplicabilityRequiresInputEquals(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_input_equals_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "mode_specific_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content", "mode"},
        .input_equals = {"mode=workflow"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto non_matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-input-equals-skip",
        .task_type = "write_file",
        .objective = "write with non-matching workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-input-equals-skip",
        .inputs = {
            {"path", "workflow/input_equals_skip.txt"},
            {"content", "direct"},
            {"mode", "direct"},
        },
    });

    Expect(non_matching_result.success, "direct write should succeed when workflow input_equals condition is not satisfied");
    Expect(non_matching_result.route_target == "file_write", "router should skip workflow when input_equals does not match");

    const auto matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-input-equals-match",
        .task_type = "write_file",
        .objective = "write with matching workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-input-equals-match",
        .inputs = {
            {"path", "workflow/input_equals_match.txt"},
            {"content", "workflow"},
            {"mode", "workflow"},
        },
    });

    Expect(matching_result.success, "workflow write should succeed when input_equals condition is satisfied");
    Expect(matching_result.route_target == "workflow_run", "router should select workflow when input_equals matches");
    Expect(matching_result.output_json.find("mode_specific_write_workflow") != std::string::npos,
        "selected input_equals workflow should be visible in output");
}

void TestRouterWorkflowApplicabilityRequiresNumericInputs(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_numeric_inputs_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "numeric_condition_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content", "priority", "size"},
        .input_number_gte = {"priority=5"},
        .input_number_lte = {"size=10"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto low_priority_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-numeric-low-priority",
        .task_type = "write_file",
        .objective = "write with priority below workflow threshold",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-numeric-low-priority",
        .inputs = {
            {"path", "workflow/numeric_low_priority.txt"},
            {"content", "direct"},
            {"priority", "4"},
            {"size", "8"},
        },
    });

    Expect(low_priority_result.success, "direct write should succeed when numeric gte condition is not satisfied");
    Expect(low_priority_result.route_target == "file_write", "router should skip workflow when numeric gte condition fails");

    const auto high_size_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-numeric-high-size",
        .task_type = "write_file",
        .objective = "write with size above workflow threshold",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-numeric-high-size",
        .inputs = {
            {"path", "workflow/numeric_high_size.txt"},
            {"content", "direct"},
            {"priority", "5"},
            {"size", "11"},
        },
    });

    Expect(high_size_result.success, "direct write should succeed when numeric lte condition is not satisfied");
    Expect(high_size_result.route_target == "file_write", "router should skip workflow when numeric lte condition fails");

    const auto matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-numeric-match",
        .task_type = "write_file",
        .objective = "write with matching numeric workflow conditions",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-numeric-match",
        .inputs = {
            {"path", "workflow/numeric_match.txt"},
            {"content", "workflow"},
            {"priority", "5.5"},
            {"size", "9.25"},
        },
    });

    Expect(matching_result.success, "workflow write should succeed when numeric conditions are satisfied");
    Expect(matching_result.route_target == "workflow_run", "router should select workflow when numeric conditions match");
    Expect(matching_result.output_json.find("numeric_condition_write_workflow") != std::string::npos,
        "selected numeric-condition workflow should be visible in output");
}

void TestRouterWorkflowApplicabilityRequiresBooleanInputs(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_boolean_inputs_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "boolean_condition_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content", "approved"},
        .input_bool = {"approved=true"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto unapproved_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-boolean-unapproved",
        .task_type = "write_file",
        .objective = "write with boolean workflow condition disabled",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-boolean-unapproved",
        .inputs = {
            {"path", "workflow/boolean_unapproved.txt"},
            {"content", "direct"},
            {"approved", "false"},
        },
    });

    Expect(unapproved_result.success, "direct write should succeed when boolean condition is not satisfied");
    Expect(unapproved_result.route_target == "file_write", "router should skip workflow when boolean condition fails");

    const auto matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-boolean-match",
        .task_type = "write_file",
        .objective = "write with matching boolean workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-boolean-match",
        .inputs = {
            {"path", "workflow/boolean_match.txt"},
            {"content", "workflow"},
            {"approved", "true"},
        },
    });

    Expect(matching_result.success, "workflow write should succeed when boolean condition is satisfied");
    Expect(matching_result.route_target == "workflow_run", "router should select workflow when boolean condition matches");
    Expect(matching_result.output_json.find("boolean_condition_write_workflow") != std::string::npos,
        "selected boolean-condition workflow should be visible in output");
}

void TestRouterWorkflowApplicabilityRequiresRegexInputs(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_regex_inputs_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "regex_condition_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content", "branch"},
        .input_regex = {"branch=release/[0-9]+\\.[0-9]+"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto non_matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-regex-non-match",
        .task_type = "write_file",
        .objective = "write with non-matching regex workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-regex-non-match",
        .inputs = {
            {"path", "workflow/regex_non_match.txt"},
            {"content", "direct"},
            {"branch", "feature/demo"},
        },
    });

    Expect(non_matching_result.success, "direct write should succeed when regex condition is not satisfied");
    Expect(non_matching_result.route_target == "file_write", "router should skip workflow when regex condition fails");

    const auto matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-regex-match",
        .task_type = "write_file",
        .objective = "write with matching regex workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-regex-match",
        .inputs = {
            {"path", "workflow/regex_match.txt"},
            {"content", "workflow"},
            {"branch", "release/12.3"},
        },
    });

    Expect(matching_result.success, "workflow write should succeed when regex condition is satisfied");
    Expect(matching_result.route_target == "workflow_run", "router should select workflow when regex condition matches");
    Expect(matching_result.output_json.find("regex_condition_write_workflow") != std::string::npos,
        "selected regex-condition workflow should be visible in output");
}

void TestRouterWorkflowApplicabilityRequiresInputAny(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_input_any_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "input_any_condition_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content"},
        .input_any = {"equals:mode=workflow|equals:mode=automated", "exists:ticket|regex:branch=release/.*"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto non_matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-input-any-non-match",
        .task_type = "write_file",
        .objective = "write with non-matching input_any workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-input-any-non-match",
        .inputs = {
            {"path", "workflow/input_any_non_match.txt"},
            {"content", "direct"},
            {"mode", "manual"},
            {"branch", "feature/demo"},
        },
    });

    Expect(non_matching_result.success, "direct write should succeed when input_any groups are not satisfied");
    Expect(non_matching_result.route_target == "file_write", "router should skip workflow when an input_any group fails");

    const auto matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-input-any-match",
        .task_type = "write_file",
        .objective = "write with matching input_any workflow condition",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-input-any-match",
        .inputs = {
            {"path", "workflow/input_any_match.txt"},
            {"content", "workflow"},
            {"mode", "automated"},
            {"ticket", "INC-123"},
        },
    });

    Expect(matching_result.success, "workflow write should succeed when every input_any group has a match");
    Expect(matching_result.route_target == "workflow_run", "router should select workflow when input_any groups match");
    Expect(matching_result.output_json.find("input_any_condition_write_workflow") != std::string::npos,
        "selected input_any workflow should be visible in output");
}

void TestRouterWorkflowApplicabilityRequiresInputExpression(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_workflow_input_expr_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "input_expr_condition_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .required_inputs = {"path", "content"},
        .input_expr = {"equals:mode=workflow && (exists:ticket || regex:branch=release/.*) && !bool:blocked=true"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });

    const auto blocked_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-input-expr-blocked",
        .task_type = "write_file",
        .objective = "write with blocked workflow expression",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-input-expr-blocked",
        .inputs = {
            {"path", "workflow/input_expr_blocked.txt"},
            {"content", "direct"},
            {"mode", "workflow"},
            {"ticket", "INC-123"},
            {"blocked", "true"},
        },
    });

    Expect(blocked_result.success, "direct write should succeed when input_expr is not satisfied");
    Expect(blocked_result.route_target == "file_write", "router should skip workflow when input_expr evaluates false");

    const auto matching_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "workflow-input-expr-match",
        .task_type = "write_file",
        .objective = "write with matching workflow expression",
        .workspace_path = isolated_workspace,
        .idempotency_key = "workflow-input-expr-match",
        .inputs = {
            {"path", "workflow/input_expr_match.txt"},
            {"content", "workflow"},
            {"mode", "workflow"},
            {"branch", "release/2026.04"},
            {"blocked", "false"},
        },
    });

    Expect(matching_result.success, "workflow write should succeed when input_expr evaluates true");
    Expect(matching_result.route_target == "workflow_run", "router should select workflow when input_expr matches");
    Expect(matching_result.output_json.find("input_expr_condition_write_workflow") != std::string::npos,
        "selected input_expr workflow should be visible in output");
}

void TestWorkflowValidationRejectsMalformedConditions() {
    const auto diagnostics = agentos::ValidateWorkflowDefinition(agentos::WorkflowDefinition{
        .name = "invalid_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .input_number_gte = {"priority=high"},
        .input_regex = {"branch=["},
        .input_any = {"unknown:mode=workflow"},
        .input_expr = {"equals:mode=workflow && ("},
        .source = "manual",
        .enabled = true,
    });

    bool saw_number = false;
    bool saw_regex = false;
    bool saw_any = false;
    bool saw_expr = false;
    for (const auto& diagnostic : diagnostics) {
        saw_number = saw_number || diagnostic.field == "input_number_gte";
        saw_regex = saw_regex || diagnostic.field == "input_regex";
        saw_any = saw_any || diagnostic.field == "input_any";
        saw_expr = saw_expr || diagnostic.field == "input_expr";
    }

    Expect(saw_number, "workflow validation should reject non-numeric numeric conditions");
    Expect(saw_regex, "workflow validation should reject invalid regex conditions");
    Expect(saw_any, "workflow validation should reject unknown input_any atom types");
    Expect(saw_expr, "workflow validation should reject malformed input_expr syntax");
}

void TestRouterSkipsWorkflowAfterRepeatedLesson(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_lesson_workflow_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.memory_manager.workflow_store().save(agentos::WorkflowDefinition{
        .name = "lesson_suppressed_write_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .source = "promoted_candidate",
        .enabled = true,
        .score = 100.0,
    });
    runtime.memory_manager.lesson_store().save(agentos::LessonRecord{
        .lesson_id = "write_file|workflow_run|WorkflowStepFailed",
        .task_type = "write_file",
        .target_name = "workflow_run",
        .error_code = "WorkflowStepFailed",
        .summary = "workflow failed repeatedly",
        .occurrence_count = 2,
        .last_task_id = "previous-workflow-failure",
        .enabled = true,
    });

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "router-lesson-suppressed-workflow",
        .task_type = "write_file",
        .objective = "write through direct skill after workflow lesson",
        .workspace_path = isolated_workspace,
        .idempotency_key = "router-lesson-suppressed-workflow",
        .inputs = {
            {"path", "workflow/lesson_suppressed.txt"},
            {"content", "direct-after-lesson"},
        },
    });

    Expect(result.success, "write should still succeed when workflow is suppressed by lesson");
    Expect(result.route_target == "file_write", "router should skip automatic workflow after repeated workflow lesson");
}

void TestRouterPenalizesAgentLessons(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "router_agent_lesson_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("lesson_agent_a", "analysis"));
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("lesson_agent_b", "analysis"));
    runtime.memory_manager.lesson_store().save(agentos::LessonRecord{
        .lesson_id = "analysis|lesson_agent_a|AgentFailed",
        .task_type = "analysis",
        .target_name = "lesson_agent_a",
        .error_code = "AgentFailed",
        .summary = "agent failed repeatedly",
        .occurrence_count = 2,
        .last_task_id = "previous-agent-failure",
        .enabled = true,
    });

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "router-agent-lesson",
        .task_type = "analysis",
        .objective = "analyze with lesson-aware routing",
        .workspace_path = isolated_workspace,
    });

    Expect(result.success, "agent task should still succeed with lesson-aware routing");
    Expect(result.route_target == "lesson_agent_b", "router should penalize agents with repeated lessons");
}

void TestPolicyDenialIncludesLessonHint(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "policy_lesson_hint_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.memory_manager.lesson_store().save(agentos::LessonRecord{
        .lesson_id = "read_file|file_read|PolicyDenied",
        .task_type = "read_file",
        .target_name = "file_read",
        .error_code = "PolicyDenied",
        .summary = "path escapes the active workspace",
        .occurrence_count = 2,
        .last_task_id = "previous-policy-denial",
        .enabled = true,
    });

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "policy-lesson-hint",
        .task_type = "read_file",
        .objective = "repeat a known policy denial",
        .workspace_path = isolated_workspace,
        .inputs = {
            {"path", "../outside.txt"},
        },
    });

    Expect(!result.success, "known policy denial should still fail");
    Expect(result.error_code == "PolicyDenied", "known policy denial should keep policy error code");
    Expect(result.error_message.find("lesson_hint=previous_policy_denials:2") != std::string::npos, "policy denial should include lesson hint");
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

    Expect(result.success, "analysis task should route to local planner");
    Expect(result.route_target == "local_planner", "default healthy agent should be local_planner");
}

void TestAgentTaskReceivesInputsAndModelConstraint(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "agent_model_constraint_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    runtime.agent_registry.register_agent(std::make_shared<StaticTestAgent>("model_capture_agent", "analysis"));

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "agent-model-constraint",
        .task_type = "analysis",
        .objective = "capture model selection",
        .workspace_path = isolated_workspace,
        .inputs = {
            {"model", "gemini-3.1-pro"},
            {"extra", "value"},
        },
        .preferred_target = std::string("model_capture_agent"),
    });

    Expect(result.success, "agent model constraint task should succeed");
    Expect(result.output_json.find("\\\"model\\\":\\\"gemini-3.1-pro\\\"") != std::string::npos,
        "agent context_json should include model input");
    Expect(result.output_json.find("\\\"extra\\\":\\\"value\\\"") != std::string::npos,
        "agent context_json should include non-model inputs");
    Expect(result.output_json.find("constraints_json") != std::string::npos &&
        result.output_json.find("\\\"model\\\":\\\"gemini-3.1-pro\\\"") != std::string::npos,
        "agent constraints_json should include selected model");
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
    Expect(!second.from_cache, "changed inputs should not replay from idempotency cache");

    auto third_request = second_request;
    third_request.task_id = "idempotent-write-replay-identical";
    const auto third = runtime.loop.run(third_request);
    Expect(third.success, "identical idempotent write should succeed");
    Expect(third.from_cache, "identical idempotent write should come from cache");

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
        .idempotency_key = "persistent-log-write",
        .inputs = {
            {"path", "logs/result.txt"},
            {"content", "log"},
        },
    });

    Expect(result.success, "persistent log write should succeed");
    Expect(std::filesystem::exists(workspace / "memory" / "task_log.tsv"), "task_log.tsv should be written");
    Expect(std::filesystem::exists(workspace / "memory" / "step_log.tsv"), "step_log.tsv should be written");
}

void TestLessonStoreRecordsFailuresAcrossRestart(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "lesson_store_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    {
        TestRuntime runtime(isolated_workspace);
        RegisterCore(runtime);

        for (int index = 0; index < 2; ++index) {
            const auto result = runtime.loop.run(agentos::TaskRequest{
                .task_id = "lesson-failure-" + std::to_string(index),
                .task_type = "read_file",
                .objective = "record repeated workspace escape failure",
                .workspace_path = isolated_workspace,
                .inputs = {
                    {"path", "../outside.txt"},
                },
            });
            Expect(!result.success, "lesson test failure should be recorded");
        }

        const auto lessons = runtime.memory_manager.lesson_store().list();
        Expect(lessons.size() == 1, "lesson store should aggregate repeated failures in memory");
        if (!lessons.empty()) {
            Expect(lessons.front().occurrence_count == 2, "lesson store should count repeated failures");
            Expect(lessons.front().error_code == "PolicyDenied", "lesson store should preserve failure code");
        }
    }

    agentos::MemoryManager reloaded_memory(isolated_workspace / "memory");
    const auto reloaded_lessons = reloaded_memory.lesson_store().list();
    Expect(reloaded_lessons.size() == 1, "lesson store should reload persisted failures");
    if (!reloaded_lessons.empty()) {
        Expect(reloaded_lessons.front().occurrence_count == 2, "lesson store should persist repeated failure count");
        Expect(reloaded_lessons.front().target_name == "file_read", "lesson store should persist target name");
        Expect(reloaded_lessons.front().summary.find("lesson_hint=") == std::string::npos, "lesson summary should not persist generated lesson hints");
    }
    Expect(std::filesystem::exists(isolated_workspace / "memory" / "lessons.tsv"), "lessons.tsv should be written");
}

void TestWorkflowCandidatesAcrossRestart(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "workflow_candidates_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    {
        TestRuntime runtime(isolated_workspace);
        RegisterCore(runtime);

        // Phase 2.2 promotion gate requires >=3 occurrences of the same step
        // signature AND >=60% ratio. Run the write_file task 3 times so the
        // canonical step sequence accumulates enough recurrences for promotion.
        for (int index = 0; index < 3; ++index) {
            const auto result = runtime.loop.run(agentos::TaskRequest{
                .task_id = "workflow-candidate-" + std::to_string(index),
                .task_type = "write_file",
                .objective = "generate repeated task pattern",
                .workspace_path = isolated_workspace,
                .idempotency_key = "workflow-candidate-" + std::to_string(index),
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
            .idempotency_key = "workflow-candidate-failure",
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
        Expect(workflow->use_count == 4, "workflow scoring should count successful and failed attempts");
        Expect(workflow->success_count == 3, "workflow scoring should count successful attempts");
        Expect(workflow->failure_count == 1, "workflow scoring should count failed attempts");
        Expect(workflow->success_rate > 0.74 && workflow->success_rate < 0.76,
            "workflow scoring should compute success rate (3/4 successful)");
        Expect(workflow->score > 0.0, "workflow scoring should produce a positive candidate score for mostly successful workflows");
        int total_signature_count = 0;
        for (const auto& [signature, count] : workflow->step_signature_counts) {
            total_signature_count += count;
        }
        Expect(total_signature_count == 3,
            "Phase 2.2: step_signature_counts must sum to the number of successful runs");
    }
    Expect(std::filesystem::exists(isolated_workspace / "memory" / "workflow_candidates.tsv"), "workflow_candidates.tsv should be written");
    Expect(std::filesystem::exists(isolated_workspace / "memory" / ".workflow_candidates_schema_version"),
        "Phase 2.2: workflow candidates schema version stamp must exist after refresh");
}

void TestWorkflowPromotionRequiresThreeRecurrences(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "promotion_two_runs_no_promote";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    for (int index = 0; index < 2; ++index) {
        const auto result = runtime.loop.run(agentos::TaskRequest{
            .task_id = "promo-2runs-" + std::to_string(index),
            .task_type = "write_file",
            .objective = "two-run pattern should not promote",
            .workspace_path = isolated_workspace,
            .idempotency_key = "promo-2runs-" + std::to_string(index),
            .inputs = {
                {"path", "promo_two_runs/" + std::to_string(index) + ".txt"},
                {"content", "x"},
            },
        });
        Expect(result.success, "two-run promotion fixture should succeed");
    }

    const auto candidates = runtime.memory_manager.workflow_candidates();
    const auto found = std::find_if(candidates.begin(), candidates.end(), [](const agentos::WorkflowCandidate& c) {
        return c.trigger_task_type == "write_file";
    });
    Expect(found == candidates.end(),
        "Phase 2.2: only 2 successful runs of the same task_type must NOT promote a workflow");
}

void TestWorkflowPromotionUnanimousThreeRuns(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "promotion_three_unanimous";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    for (int index = 0; index < 3; ++index) {
        const auto result = runtime.loop.run(agentos::TaskRequest{
            .task_id = "promo-unanimous-" + std::to_string(index),
            .task_type = "write_file",
            .objective = "unanimous-three pattern should promote",
            .workspace_path = isolated_workspace,
            .idempotency_key = "promo-unanimous-" + std::to_string(index),
            .inputs = {
                {"path", "promo_unanimous/" + std::to_string(index) + ".txt"},
                {"content", "x"},
            },
        });
        Expect(result.success, "unanimous-three promotion fixture should succeed");
    }

    const auto candidates = runtime.memory_manager.workflow_candidates();
    const auto found = std::find_if(candidates.begin(), candidates.end(), [](const agentos::WorkflowCandidate& c) {
        return c.trigger_task_type == "write_file";
    });
    Expect(found != candidates.end(),
        "Phase 2.2: 3 successful runs with identical step sequences MUST promote a workflow");
    if (found != candidates.end()) {
        Expect(found->success_count == 3, "promoted candidate should record all three successes");
        Expect(found->step_signature_counts.size() == 1,
            "unanimous fixture should produce exactly one step signature");
        const auto signature_count = found->step_signature_counts.begin()->second;
        Expect(signature_count == 3,
            "Phase 2.2: the unanimous signature must record 3 occurrences");
    }
}

void RecordSyntheticWorkflowRun(
    agentos::MemoryManager& memory_manager,
    const std::string& task_id,
    const std::string& task_type,
    const std::vector<std::string>& target_names) {
    agentos::TaskRunResult result{
        .success = true,
        .summary = "synthetic success",
        .route_target = target_names.empty() ? "" : target_names.front(),
        .route_kind = target_names.empty() ? agentos::RouteTargetKind::none : agentos::RouteTargetKind::skill,
        .duration_ms = 10,
    };
    for (const auto& target_name : target_names) {
        result.steps.push_back(agentos::TaskStepRecord{
            .target_kind = agentos::RouteTargetKind::skill,
            .target_name = target_name,
            .success = true,
            .duration_ms = 1,
            .summary = "synthetic step",
        });
    }
    memory_manager.record_task(
        agentos::TaskRequest{
            .task_id = task_id,
            .task_type = task_type,
            .objective = "synthetic workflow promotion fixture",
            .idempotency_key = task_id,
        },
        result);
}

void TestWorkflowPromotionMajoritySignatureWins(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "promotion_majority_signature";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::MemoryManager memory_manager(isolated_workspace / "memory");
    const std::string task_type = "synthetic_majority";
    for (int index = 0; index < 3; ++index) {
        RecordSyntheticWorkflowRun(
            memory_manager,
            "majority-a-" + std::to_string(index),
            task_type,
            {"file_write", "file_read"});
    }
    for (int index = 0; index < 2; ++index) {
        RecordSyntheticWorkflowRun(
            memory_manager,
            "majority-b-" + std::to_string(index),
            task_type,
            {"file_write", "file_patch", "file_read"});
    }

    const auto candidates = memory_manager.workflow_candidates();
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const agentos::WorkflowCandidate& c) {
        return c.trigger_task_type == task_type;
    });
    Expect(found != candidates.end(),
        "Phase 2.2: a 3-of-5 majority signature at 60 percent must promote a workflow");
    if (found != candidates.end()) {
        Expect(found->success_count == 5, "majority candidate should record all successful runs");
        Expect(found->ordered_steps == std::vector<std::string>({"file_write", "file_read"}),
            "majority candidate should use the highest-count step signature");
        Expect(found->step_signature_counts.size() == 2,
            "majority candidate should retain both observed step signatures");
        const auto majority_count = found->step_signature_counts.find("file_write|file_read");
        Expect(majority_count != found->step_signature_counts.end() && majority_count->second == 3,
            "majority signature should record 3 occurrences");
    }
}

void TestWorkflowPromotionNoMajorityDoesNotPromote(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "promotion_no_majority";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::MemoryManager memory_manager(isolated_workspace / "memory");
    const std::string task_type = "synthetic_no_majority";
    for (int index = 0; index < 3; ++index) {
        RecordSyntheticWorkflowRun(
            memory_manager,
            "no-majority-a-" + std::to_string(index),
            task_type,
            {"file_write", "file_read"});
        RecordSyntheticWorkflowRun(
            memory_manager,
            "no-majority-b-" + std::to_string(index),
            task_type,
            {"file_write", "file_patch", "file_read"});
    }

    const auto candidates = memory_manager.workflow_candidates();
    const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const agentos::WorkflowCandidate& c) {
        return c.trigger_task_type == task_type;
    });
    Expect(found == candidates.end(),
        "Phase 2.2: two 3-of-6 signatures at 50 percent must NOT promote a workflow");
}

void TestWorkflowStorePersistsDefinitions(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "workflow_store_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto store_path = isolated_workspace / "memory" / "workflows.tsv";

    auto candidate_definition = agentos::WorkflowStore::FromCandidate(agentos::WorkflowCandidate{
        .name = "write_file_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write", "file_read"},
        .use_count = 3,
        .success_count = 2,
        .failure_count = 1,
        .success_rate = 0.66,
        .avg_duration_ms = 12.5,
        .score = 123.0,
    });
    candidate_definition.required_inputs = {"path", "content"};
    candidate_definition.input_equals = {"mode=workflow"};
    candidate_definition.input_number_gte = {"priority=5"};
    candidate_definition.input_number_lte = {"size=10"};
    candidate_definition.input_bool = {"approved=true"};
    candidate_definition.input_regex = {"branch=release/.*"};
    candidate_definition.input_any = {"equals:mode=workflow|equals:mode=automated"};
    candidate_definition.input_expr = {"equals:mode=workflow&&(exists:ticket||regex:branch=release/.*)"};

    {
        agentos::WorkflowStore store(store_path);
        auto saved = candidate_definition;
        saved.enabled = true;
        store.save(saved);

        const auto found = store.find("write_file_workflow");
        Expect(found.has_value(), "workflow store should find saved workflow definition");
        if (found.has_value()) {
            Expect(found->source == "candidate", "workflow store should preserve source");
            Expect(found->enabled, "workflow store should preserve enabled flag");
            Expect(found->ordered_steps.size() == 2, "workflow store should preserve ordered steps");
            Expect(found->ordered_steps.front() == "file_write", "workflow store should preserve first step");
            Expect(found->required_inputs.size() == 2, "workflow store should preserve required inputs");
            Expect(found->input_equals.size() == 1, "workflow store should preserve input_equals conditions");
            Expect(found->input_number_gte.size() == 1, "workflow store should preserve input_number_gte conditions");
            Expect(found->input_number_lte.size() == 1, "workflow store should preserve input_number_lte conditions");
            Expect(found->input_bool.size() == 1, "workflow store should preserve input_bool conditions");
            Expect(found->input_regex.size() == 1, "workflow store should preserve input_regex conditions");
            Expect(found->input_any.size() == 1, "workflow store should preserve input_any conditions");
            Expect(found->input_expr.size() == 1, "workflow store should preserve input_expr conditions");
        }
    }

    agentos::WorkflowStore reloaded(store_path);
    const auto reloaded_definition = reloaded.find("write_file_workflow");
    Expect(reloaded_definition.has_value(), "workflow store should reload persisted workflow definition");
    if (reloaded_definition.has_value()) {
        Expect(reloaded_definition->success_count == 2, "workflow store should preserve success count");
        Expect(reloaded_definition->failure_count == 1, "workflow store should preserve failure count");
        Expect(reloaded_definition->score == 123.0, "workflow store should preserve score");
        Expect(reloaded_definition->required_inputs.size() == 2, "workflow store should reload required inputs");
        Expect(reloaded_definition->input_equals.size() == 1 &&
            reloaded_definition->input_equals.front() == "mode=workflow",
            "workflow store should reload input_equals conditions");
        Expect(reloaded_definition->input_number_gte.size() == 1 &&
            reloaded_definition->input_number_gte.front() == "priority=5",
            "workflow store should reload input_number_gte conditions");
        Expect(reloaded_definition->input_number_lte.size() == 1 &&
            reloaded_definition->input_number_lte.front() == "size=10",
            "workflow store should reload input_number_lte conditions");
        Expect(reloaded_definition->input_bool.size() == 1 &&
            reloaded_definition->input_bool.front() == "approved=true",
            "workflow store should reload input_bool conditions");
        Expect(reloaded_definition->input_regex.size() == 1 &&
            reloaded_definition->input_regex.front() == "branch=release/.*",
            "workflow store should reload input_regex conditions");
        Expect(reloaded_definition->input_any.size() == 1 &&
            reloaded_definition->input_any.front() == "equals:mode=workflow|equals:mode=automated",
            "workflow store should reload input_any conditions");
        Expect(reloaded_definition->input_expr.size() == 1 &&
            reloaded_definition->input_expr.front() == "equals:mode=workflow&&(exists:ticket||regex:branch=release/.*)",
            "workflow store should reload input_expr conditions");
    }
    Expect(std::filesystem::exists(store_path), "workflows.tsv should be written");

    reloaded.save(agentos::WorkflowDefinition{
        .name = "write_file_workflow",
        .trigger_task_type = "write_file",
        .ordered_steps = {"file_write"},
        .source = "manual",
        .enabled = false,
    });
    Expect(reloaded.list().size() == 1, "workflow store should replace existing workflow by name");
    const auto updated = reloaded.find("write_file_workflow");
    Expect(updated.has_value() && !updated->enabled && updated->source == "manual", "workflow store should persist replacement fields");

    Expect(reloaded.remove("write_file_workflow"), "workflow store should remove workflow definition");
    Expect(!reloaded.find("write_file_workflow").has_value(), "workflow store should not find removed workflow");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestWorkflowRun(workspace);
    TestWorkflowRunStoredDefinition(workspace);
    TestRouterPrefersPromotedWorkflow(workspace);
    TestRouterWorkflowApplicabilityRequiresInputs(workspace);
    TestRouterWorkflowApplicabilityRequiresInputEquals(workspace);
    TestRouterWorkflowApplicabilityRequiresNumericInputs(workspace);
    TestRouterWorkflowApplicabilityRequiresBooleanInputs(workspace);
    TestRouterWorkflowApplicabilityRequiresRegexInputs(workspace);
    TestRouterWorkflowApplicabilityRequiresInputAny(workspace);
    TestRouterWorkflowApplicabilityRequiresInputExpression(workspace);
    TestWorkflowValidationRejectsMalformedConditions();
    TestRouterSkipsWorkflowAfterRepeatedLesson(workspace);
    TestRouterPenalizesAgentLessons(workspace);
    TestPolicyDenialIncludesLessonHint(workspace);
    TestDefaultAgentRoute(workspace);
    TestAgentTaskReceivesInputsAndModelConstraint(workspace);
    TestIdempotentExecutionCache(workspace);
    TestPersistentTaskAndStepLogs(workspace);
    TestLessonStoreRecordsFailuresAcrossRestart(workspace);
    TestWorkflowCandidatesAcrossRestart(workspace);
    TestWorkflowPromotionRequiresThreeRecurrences(workspace);
    TestWorkflowPromotionUnanimousThreeRuns(workspace);
    TestWorkflowPromotionMajoritySignatureWins(workspace);
    TestWorkflowPromotionNoMajorityDoesNotPromote(workspace);
    TestWorkflowStorePersistsDefinitions(workspace);

    if (failures != 0) {
        std::cerr << failures << " workflow/router test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_workflow_router_tests passed\n";
    return 0;
}
