#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
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

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_file_skill_policy_tests";
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

class SchemaTypeTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_type_probe",
            .version = "test",
            .description = "Validate schema property types.",
            .capabilities = {"test"},
            .input_schema_json = R"({"type":"object","properties":{"count":{"type":"integer","minimum":1,"maximum":5},"ratio":{"type":"number","exclusiveMinimum":0,"exclusiveMaximum":10,"multipleOf":0.25},"enabled":{"type":"boolean"},"label":{"type":"string","enum":["ok","fine"],"minLength":2,"maxLength":8},"code":{"type":"string","pattern":"^[A-Z]{2}[0-9]{2}$"},"mode":{"type":"string","const":"strict"}},"required":["count","ratio","enabled","label","code","mode"],"additionalProperties":false})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

class SchemaDependencyTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_dependency_probe",
            .version = "test",
            .description = "Validate schema dependentRequired fields.",
            .capabilities = {"test"},
            .input_schema_json =
                R"({"type":"object","properties":{"mode":{"type":"string"},"token":{"type":"string"},"signature":{"type":"string"},"receipt":{"type":"string"}},"dependentRequired":{"token":["signature"]},"dependencies":{"receipt":["mode"]}})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

class SchemaPropertiesCountTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_properties_count_probe",
            .version = "test",
            .description = "Validate schema minProperties and maxProperties.",
            .capabilities = {"test"},
            .input_schema_json =
                R"({"type":"object","properties":{"first":{"type":"string"},"second":{"type":"string"},"third":{"type":"string"}},"minProperties":2,"maxProperties":2})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

class SchemaNotTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_not_probe",
            .version = "test",
            .description = "Validate schema not.required fields.",
            .capabilities = {"test"},
            .input_schema_json =
                R"({"type":"object","properties":{"token":{"type":"string"},"anonymous":{"type":"boolean"},"mode":{"type":"string"}},"not":{"required":["token","anonymous"]}})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

class SchemaPropertyNamesTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_property_names_probe",
            .version = "test",
            .description = "Validate schema propertyNames constraints.",
            .capabilities = {"test"},
            .input_schema_json =
                R"({"type":"object","propertyNames":{"pattern":"^[a-z_]+$","minLength":2,"maxLength":12}})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

class SchemaConditionalTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_conditional_probe",
            .version = "test",
            .description = "Validate schema if/then/else constraints.",
            .capabilities = {"test"},
            .input_schema_json =
                R"({"type":"object","properties":{"mode":{"type":"string"},"token":{"type":"string"},"anonymous_reason":{"type":"string"}},"if":{"properties":{"mode":{"const":"auth"}}},"then":{"required":["token"]},"else":{"required":["anonymous_reason"]}})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

class SchemaCombinatorTestSkill final : public agentos::ISkillAdapter {
public:
    agentos::SkillManifest manifest() const override {
        return {
            .name = "schema_combinator_probe",
            .version = "test",
            .description = "Validate schema combinator required branches.",
            .capabilities = {"test"},
            .input_schema_json =
                R"({"type":"object","properties":{"email":{"type":"string"},"phone":{"type":"string"},"api_key":{"type":"string"},"oauth_token":{"type":"string"},"workspace":{"type":"string"},"project":{"type":"string"}},"anyOf":[{"required":["email"]},{"required":["phone"]}],"oneOf":[{"required":["api_key"]},{"required":["oauth_token"]}],"allOf":[{"required":["workspace"]},{"required":["project"]}]})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
            .supports_streaming = false,
            .idempotent = true,
            .timeout_ms = 1000,
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall& call) override {
        (void)call;
        return {
            .success = true,
            .json_output = "{}",
        };
    }

    bool healthy() const override {
        return true;
    }
};

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void TestFileSkillLoop(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto write_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "write",
        .task_type = "write_file",
        .objective = "write a smoke test file",
        .workspace_path = workspace,
        .idempotency_key = "write-file-skill-loop",
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

void TestPolicyRequiresIdempotencyKeyForSideEffects(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "write-without-idempotency",
        .task_type = "write_file",
        .objective = "write without idempotency key",
        .workspace_path = workspace,
        .inputs = {
            {"path", "idempotency/missing.txt"},
            {"content", "unsafe"},
        },
    });

    Expect(!result.success, "side-effecting write without idempotency_key should be denied");
    Expect(result.error_code == "PolicyDenied", "missing idempotency_key should fail at policy layer");
    Expect(result.error_message == "side-effecting skills require idempotency_key",
        "missing idempotency_key should return a clear policy reason");
    Expect(!std::filesystem::exists(workspace / "idempotency" / "missing.txt"),
        "denied side-effecting write should not create a file");
}

void TestSkillInputSchemaValidation(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-missing-required",
        .task_type = "read_file",
        .objective = "read without required path input",
        .workspace_path = workspace,
    });

    Expect(!result.success, "missing required skill input should fail schema validation");
    Expect(result.error_code == "SchemaValidationFailed", "missing required input should return SchemaValidationFailed");
    Expect(result.error_message.find("path") != std::string::npos,
        "schema validation error should name the missing required field");
    Expect(!result.steps.empty(), "schema validation failure should record a failed step");
    Expect(result.steps.front().error_code == "SchemaValidationFailed",
        "schema validation step should carry SchemaValidationFailed");
}

void TestSkillInputSchemaTypeValidation(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);
    runtime.skill_registry.register_skill(std::make_shared<SchemaTypeTestSkill>());
    runtime.skill_registry.register_skill(std::make_shared<SchemaDependencyTestSkill>());
    runtime.skill_registry.register_skill(std::make_shared<SchemaPropertiesCountTestSkill>());
    runtime.skill_registry.register_skill(std::make_shared<SchemaNotTestSkill>());
    runtime.skill_registry.register_skill(std::make_shared<SchemaPropertyNamesTestSkill>());
    runtime.skill_registry.register_skill(std::make_shared<SchemaConditionalTestSkill>());
    runtime.skill_registry.register_skill(std::make_shared<SchemaCombinatorTestSkill>());

    const auto invalid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-type",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema property type",
        .workspace_path = workspace,
        .inputs = {
            {"count", "1.5"},
            {"ratio", "2.25"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!invalid_result.success, "invalid skill input type should fail schema validation");
    Expect(invalid_result.error_code == "SchemaValidationFailed", "invalid input type should return SchemaValidationFailed");
    Expect(invalid_result.error_message.find("count:integer") != std::string::npos,
        "schema validation error should name the invalid field and expected type");

    const auto enum_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-enum",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema enum",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.25"},
            {"enabled", "true"},
            {"label", "bad"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!enum_result.success, "invalid enum input should fail schema validation");
    Expect(enum_result.error_code == "SchemaValidationFailed", "invalid enum input should return SchemaValidationFailed");
    Expect(enum_result.error_message.find("label:enum") != std::string::npos,
        "schema validation error should name the invalid enum field");

    const auto length_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-length",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema string length",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.25"},
            {"enabled", "true"},
            {"label", "x"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!length_result.success, "invalid string length input should fail schema validation");
    Expect(length_result.error_code == "SchemaValidationFailed", "invalid string length should return SchemaValidationFailed");
    Expect(length_result.error_message.find("label:minLength") != std::string::npos,
        "schema validation error should name the invalid minLength field");

    const auto range_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-range",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema numeric range",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "11"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!range_result.success, "invalid numeric range input should fail schema validation");
    Expect(range_result.error_code == "SchemaValidationFailed", "invalid numeric range should return SchemaValidationFailed");
    Expect(range_result.error_message.find("ratio:exclusiveMaximum") != std::string::npos,
        "schema validation error should name the invalid exclusiveMaximum field");

    const auto pattern_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-pattern",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema pattern",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.25"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "bad"},
            {"mode", "strict"},
        },
    });

    Expect(!pattern_result.success, "invalid pattern input should fail schema validation");
    Expect(pattern_result.error_code == "SchemaValidationFailed", "invalid pattern should return SchemaValidationFailed");
    Expect(pattern_result.error_message.find("code:pattern") != std::string::npos,
        "schema validation error should name the invalid pattern field");

    const auto multiple_of_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-multiple-of",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema multipleOf",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.3"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!multiple_of_result.success, "invalid multipleOf input should fail schema validation");
    Expect(multiple_of_result.error_code == "SchemaValidationFailed", "invalid multipleOf should return SchemaValidationFailed");
    Expect(multiple_of_result.error_message.find("ratio:multipleOf") != std::string::npos,
        "schema validation error should name the invalid multipleOf field");

    const auto exclusive_minimum_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-exclusive-minimum",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema exclusiveMinimum",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "0"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!exclusive_minimum_result.success, "invalid exclusiveMinimum input should fail schema validation");
    Expect(exclusive_minimum_result.error_code == "SchemaValidationFailed",
        "invalid exclusiveMinimum should return SchemaValidationFailed");
    Expect(exclusive_minimum_result.error_message.find("ratio:exclusiveMinimum") != std::string::npos,
        "schema validation error should name the invalid exclusiveMinimum field");

    const auto exclusive_maximum_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-exclusive-maximum",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema exclusiveMaximum",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "10"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(!exclusive_maximum_result.success, "invalid exclusiveMaximum input should fail schema validation");
    Expect(exclusive_maximum_result.error_code == "SchemaValidationFailed",
        "invalid exclusiveMaximum should return SchemaValidationFailed");
    Expect(exclusive_maximum_result.error_message.find("ratio:exclusiveMaximum") != std::string::npos,
        "schema validation error should name the invalid exclusiveMaximum field");

    const auto const_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-const",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema const",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.25"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "loose"},
        },
    });

    Expect(!const_result.success, "invalid const input should fail schema validation");
    Expect(const_result.error_code == "SchemaValidationFailed", "invalid const should return SchemaValidationFailed");
    Expect(const_result.error_message.find("mode:const") != std::string::npos,
        "schema validation error should name the invalid const field");

    const auto additional_properties_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-additional-properties",
        .task_type = "schema_type_probe",
        .objective = "validate invalid schema additionalProperties",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.25"},
            {"enabled", "true"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
            {"extra", "unexpected"},
        },
    });

    Expect(!additional_properties_result.success, "unexpected input should fail additionalProperties validation");
    Expect(additional_properties_result.error_code == "SchemaValidationFailed",
        "unexpected input should return SchemaValidationFailed");
    Expect(additional_properties_result.error_message.find("extra:additionalProperties") != std::string::npos,
        "schema validation error should name the unexpected input field");

    const auto valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-types",
        .task_type = "schema_type_probe",
        .objective = "validate accepted schema property types",
        .workspace_path = workspace,
        .inputs = {
            {"count", "2"},
            {"ratio", "2.25"},
            {"enabled", "false"},
            {"label", "ok"},
            {"code", "AB12"},
            {"mode", "strict"},
        },
    });

    Expect(valid_result.success, "valid string-encoded schema property types should pass validation");

    const auto dependent_required_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-dependent-required",
        .task_type = "schema_dependency_probe",
        .objective = "validate dependentRequired schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"token", "abc"},
        },
    });

    Expect(!dependent_required_result.success, "missing dependentRequired input should fail schema validation");
    Expect(dependent_required_result.error_code == "SchemaValidationFailed",
        "missing dependentRequired input should return SchemaValidationFailed");
    Expect(dependent_required_result.error_message.find("token:dependentRequired:signature") != std::string::npos,
        "schema validation error should name the missing dependentRequired field");

    const auto dependency_alias_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-dependencies",
        .task_type = "schema_dependency_probe",
        .objective = "validate legacy dependencies schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"receipt", "r-1"},
        },
    });

    Expect(!dependency_alias_result.success, "missing dependencies input should fail schema validation");
    Expect(dependency_alias_result.error_code == "SchemaValidationFailed",
        "missing dependencies input should return SchemaValidationFailed");
    Expect(dependency_alias_result.error_message.find("receipt:dependentRequired:mode") != std::string::npos,
        "schema validation error should name the missing dependencies field");

    const auto dependency_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-dependencies",
        .task_type = "schema_dependency_probe",
        .objective = "validate accepted dependency schema keywords",
        .workspace_path = workspace,
        .inputs = {
            {"token", "abc"},
            {"signature", "sig"},
            {"receipt", "r-1"},
            {"mode", "strict"},
        },
    });

    Expect(dependency_valid_result.success, "valid dependentRequired and dependencies inputs should pass validation");

    const auto min_properties_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-min-properties",
        .task_type = "schema_properties_count_probe",
        .objective = "validate minProperties schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"first", "a"},
        },
    });

    Expect(!min_properties_result.success, "too few input properties should fail schema validation");
    Expect(min_properties_result.error_code == "SchemaValidationFailed",
        "too few input properties should return SchemaValidationFailed");
    Expect(min_properties_result.error_message.find("schema:minProperties") != std::string::npos,
        "schema validation error should name minProperties failures");

    const auto max_properties_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-max-properties",
        .task_type = "schema_properties_count_probe",
        .objective = "validate maxProperties schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"first", "a"},
            {"second", "b"},
            {"third", "c"},
        },
    });

    Expect(!max_properties_result.success, "too many input properties should fail schema validation");
    Expect(max_properties_result.error_code == "SchemaValidationFailed",
        "too many input properties should return SchemaValidationFailed");
    Expect(max_properties_result.error_message.find("schema:maxProperties") != std::string::npos,
        "schema validation error should name maxProperties failures");

    const auto properties_count_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-properties-count",
        .task_type = "schema_properties_count_probe",
        .objective = "validate accepted properties count",
        .workspace_path = workspace,
        .inputs = {
            {"first", "a"},
            {"second", "b"},
        },
    });

    Expect(properties_count_valid_result.success, "valid minProperties and maxProperties inputs should pass validation");

    const auto not_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-not",
        .task_type = "schema_not_probe",
        .objective = "validate not.required schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"token", "abc"},
            {"anonymous", "true"},
        },
    });

    Expect(!not_result.success, "input matching not.required should fail schema validation");
    Expect(not_result.error_code == "SchemaValidationFailed",
        "input matching not.required should return SchemaValidationFailed");
    Expect(not_result.error_message.find("schema:not:required") != std::string::npos,
        "schema validation error should name not.required failures");

    const auto not_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-not",
        .task_type = "schema_not_probe",
        .objective = "validate accepted not.required schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"token", "abc"},
        },
    });

    Expect(not_valid_result.success, "input not matching not.required should pass validation");

    const auto property_names_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-property-names",
        .task_type = "schema_property_names_probe",
        .objective = "validate propertyNames schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"BadName", "abc"},
        },
    });

    Expect(!property_names_result.success, "invalid propertyNames input should fail schema validation");
    Expect(property_names_result.error_code == "SchemaValidationFailed",
        "invalid propertyNames input should return SchemaValidationFailed");
    Expect(property_names_result.error_message.find("schema:propertyNames:pattern:BadName") != std::string::npos,
        "schema validation error should name propertyNames pattern failures");

    const auto property_names_length_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-property-names-length",
        .task_type = "schema_property_names_probe",
        .objective = "validate propertyNames length schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"x", "abc"},
        },
    });

    Expect(!property_names_length_result.success, "invalid propertyNames length should fail schema validation");
    Expect(property_names_length_result.error_code == "SchemaValidationFailed",
        "invalid propertyNames length should return SchemaValidationFailed");
    Expect(property_names_length_result.error_message.find("schema:propertyNames:minLength:x") != std::string::npos,
        "schema validation error should name propertyNames minLength failures");

    const auto property_names_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-property-names",
        .task_type = "schema_property_names_probe",
        .objective = "validate accepted propertyNames schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"valid_name", "abc"},
        },
    });

    Expect(property_names_valid_result.success, "valid propertyNames input should pass validation");

    const auto conditional_then_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-conditional-then",
        .task_type = "schema_conditional_probe",
        .objective = "validate if/then schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"mode", "auth"},
        },
    });

    Expect(!conditional_then_result.success, "missing then-required input should fail schema validation");
    Expect(conditional_then_result.error_code == "SchemaValidationFailed",
        "missing then-required input should return SchemaValidationFailed");
    Expect(conditional_then_result.error_message.find("schema:then:required:token") != std::string::npos,
        "schema validation error should name then required failures");

    const auto conditional_else_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-conditional-else",
        .task_type = "schema_conditional_probe",
        .objective = "validate if/else schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"mode", "guest"},
        },
    });

    Expect(!conditional_else_result.success, "missing else-required input should fail schema validation");
    Expect(conditional_else_result.error_code == "SchemaValidationFailed",
        "missing else-required input should return SchemaValidationFailed");
    Expect(conditional_else_result.error_message.find("schema:else:required:anonymous_reason") != std::string::npos,
        "schema validation error should name else required failures");

    const auto conditional_then_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-conditional-then",
        .task_type = "schema_conditional_probe",
        .objective = "validate accepted if/then schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"mode", "auth"},
            {"token", "abc"},
        },
    });

    Expect(conditional_then_valid_result.success, "valid then-required input should pass validation");

    const auto conditional_else_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-conditional-else",
        .task_type = "schema_conditional_probe",
        .objective = "validate accepted if/else schema keyword",
        .workspace_path = workspace,
        .inputs = {
            {"mode", "guest"},
            {"anonymous_reason", "readonly"},
        },
    });

    Expect(conditional_else_valid_result.success, "valid else-required input should pass validation");

    const auto any_of_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-any-of",
        .task_type = "schema_combinator_probe",
        .objective = "validate anyOf required branches",
        .workspace_path = workspace,
        .inputs = {
            {"api_key", "key"},
            {"workspace", "main"},
            {"project", "agentos"},
        },
    });

    Expect(!any_of_result.success, "input matching no anyOf branch should fail schema validation");
    Expect(any_of_result.error_code == "SchemaValidationFailed",
        "input matching no anyOf branch should return SchemaValidationFailed");
    Expect(any_of_result.error_message.find("schema:anyOf:required") != std::string::npos,
        "schema validation error should name anyOf required failures");

    const auto one_of_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-one-of",
        .task_type = "schema_combinator_probe",
        .objective = "validate oneOf required branches",
        .workspace_path = workspace,
        .inputs = {
            {"email", "dev@example.test"},
            {"api_key", "key"},
            {"oauth_token", "token"},
            {"workspace", "main"},
            {"project", "agentos"},
        },
    });

    Expect(!one_of_result.success, "input matching multiple oneOf branches should fail schema validation");
    Expect(one_of_result.error_code == "SchemaValidationFailed",
        "input matching multiple oneOf branches should return SchemaValidationFailed");
    Expect(one_of_result.error_message.find("schema:oneOf:required") != std::string::npos,
        "schema validation error should name oneOf required failures");

    const auto all_of_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-invalid-all-of",
        .task_type = "schema_combinator_probe",
        .objective = "validate allOf required branches",
        .workspace_path = workspace,
        .inputs = {
            {"phone", "555-0100"},
            {"oauth_token", "token"},
            {"workspace", "main"},
        },
    });

    Expect(!all_of_result.success, "input missing an allOf branch requirement should fail schema validation");
    Expect(all_of_result.error_code == "SchemaValidationFailed",
        "input missing an allOf branch requirement should return SchemaValidationFailed");
    Expect(all_of_result.error_message.find("schema:allOf:required") != std::string::npos,
        "schema validation error should name allOf required failures");

    const auto combinator_valid_result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "schema-valid-combinators",
        .task_type = "schema_combinator_probe",
        .objective = "validate accepted schema combinator keywords",
        .workspace_path = workspace,
        .inputs = {
            {"phone", "555-0100"},
            {"oauth_token", "token"},
            {"workspace", "main"},
            {"project", "agentos"},
        },
    });

    Expect(combinator_valid_result.success, "valid combinator required branch inputs should pass validation");
}

void TestAuditRedactsSensitiveTaskValues(const std::filesystem::path& workspace) {
    const auto audit_path = workspace / "secret_redaction_audit.log";
    agentos::AuditLogger audit_logger(audit_path);
    const std::string secret = "audit-super-secret-token";

    const agentos::TaskRequest task{
        .task_id = "audit-secret-redaction",
        .task_type = "analysis",
        .objective = "use api key " + secret,
        .workspace_path = workspace,
        .inputs = {
            {"api_key", secret},
        },
    };

    audit_logger.record_task_start(task);
    audit_logger.record_policy(task.task_id, "local_planner", agentos::PolicyDecision{
        .allowed = false,
        .reason = "policy saw " + secret,
    });
    audit_logger.record_step(task.task_id, agentos::TaskStepRecord{
        .target_kind = agentos::RouteTargetKind::agent,
        .target_name = "local_planner",
        .success = false,
        .summary = "step used " + secret,
        .error_message = "step failed with " + secret,
    });
    audit_logger.record_task_end(task.task_id, agentos::TaskRunResult{
        .success = false,
        .summary = "task ended with " + secret,
        .error_message = "task failed with " + secret,
    });

    std::ifstream input(audit_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto audit_text = buffer.str();

    Expect(audit_text.find(secret) == std::string::npos, "audit log should not contain sensitive task values");
    Expect(audit_text.find("[REDACTED]") != std::string::npos, "audit log should include redaction markers");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestFileSkillLoop(workspace);
    TestPolicyRequiresIdempotencyKeyForSideEffects(workspace);
    TestSkillInputSchemaValidation(workspace);
    TestSkillInputSchemaTypeValidation(workspace);
    TestAuditRedactsSensitiveTaskValues(workspace);

    if (failures != 0) {
        std::cerr << failures << " file-skill/policy test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_file_skill_policy_tests passed\n";
    return 0;
}
