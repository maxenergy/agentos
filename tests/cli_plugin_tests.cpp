#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/local_planning_agent.hpp"
#include "hosts/cli/cli_host.hpp"
#include "hosts/cli/cli_skill_invoker.hpp"
#include "hosts/cli/cli_spec_loader.hpp"
#include "hosts/plugin/plugin_host.hpp"
#include "memory/memory_manager.hpp"
#include "test_command_fixtures.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using agentos::test::PrependPathForTest;
using agentos::test::ReadEnvForTest;
using agentos::test::ScopedEnvOverride;
using agentos::test::SetEnvForTest;
using agentos::test::WriteCliFixture;
using agentos::test::WriteJqCliFixture;

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

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

agentos::CliSpec MakeSensitiveEchoSpec() {
#ifdef _WIN32
    return {
        .name = "sensitive_echo",
        .description = "Echo a sensitive argument to verify CLI redaction.",
        .binary = "cmd",
        .args_template = {"/d", "/s", "/c", "echo key={{api_key}}"},
        .required_args = {"api_key"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 3000,
        .output_limit_bytes = 4096,
    };
#else
    return {
        .name = "sensitive_echo",
        .description = "Echo a sensitive argument to verify CLI redaction.",
        .binary = "sh",
        .args_template = {"-c", "printf '%s\\n' \"key={{api_key}}\""},
        .required_args = {"api_key"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 3000,
        .output_limit_bytes = 4096,
    };
#endif
}

agentos::CliSpec MakeProcessLimitProbeSpec() {
#ifdef _WIN32
    return {
        .name = "process_limit_probe",
        .description = "Attempt to spawn a second process while max_processes is capped at one.",
        .binary = "powershell",
        .args_template = {
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "try { Start-Process -FilePath powershell -ArgumentList '-NoProfile','-NonInteractive','-Command','exit 0' -Wait -ErrorAction Stop | Out-Null; Write-Output 'spawned'; exit 0 } catch { Write-Output 'spawn denied'; exit 42 }",
        },
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 3000,
        .output_limit_bytes = 4096,
        .max_processes = 1,
    };
#else
    return {
        .name = "process_limit_probe",
        .description = "No-op process limit probe on platforms without enforced CLI process caps.",
        .binary = "sh",
        .args_template = {"-c", "printf '%s\\n' 'resource-limit-skip'"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 3000,
        .output_limit_bytes = 4096,
        .max_processes = 1,
    };
#endif
}

agentos::CliSpec MakeTimeoutProcessTreeSpec() {
#ifdef _WIN32
    return {
        .name = "timeout_process_tree",
        .description = "Start a child process that should be killed with its parent on timeout.",
        .binary = "cmd",
        .args_template = {"/d", "/s", "/c", "powershell -NoProfile -NonInteractive -Command Start-Sleep -Seconds 5"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 300,
        .output_limit_bytes = 4096,
    };
#else
    return {
        .name = "timeout_process_tree",
        .description = "Start a child process that should be killed with its parent on timeout.",
        .binary = "sh",
        .args_template = {"-c", "sleep 5"},
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"process.spawn"},
        .timeout_ms = 300,
        .output_limit_bytes = 4096,
    };
#endif
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_cli_plugin_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
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

void TestCliHostRedactsSensitiveArguments(const std::filesystem::path& workspace) {
    agentos::CliHost cli_host;
    const auto result = cli_host.run(agentos::CliRunRequest{
        .spec = MakeSensitiveEchoSpec(),
        .arguments = {
            {"api_key", "super-secret-cli-token"},
        },
        .workspace_path = workspace,
    });

    Expect(result.success, "CLI sensitive echo should execute successfully");
    Expect(result.command_display.find("super-secret-cli-token") == std::string::npos,
        "CLI command display should redact sensitive argument values");
    Expect(result.stdout_text.find("super-secret-cli-token") == std::string::npos,
        "CLI stdout should redact sensitive argument values");
    Expect(result.command_display.find("[REDACTED]") != std::string::npos,
        "CLI command display should show a redaction marker");
    Expect(result.stdout_text.find("[REDACTED]") != std::string::npos,
        "CLI stdout should show a redaction marker");
}

void TestCliHostProcessLimit(const std::filesystem::path& workspace) {
    agentos::CliHost cli_host;
    const auto result = cli_host.run(agentos::CliRunRequest{
        .spec = MakeProcessLimitProbeSpec(),
        .workspace_path = workspace,
    });

#ifdef _WIN32
    Expect(!result.success, "CLI process-limit probe should fail when a second process exceeds max_processes");
    Expect(result.exit_code != 0, "CLI process-limit probe should surface a non-zero exit when the limit is hit");
    Expect(result.stdout_text.find("spawn denied") != std::string::npos,
        "CLI process-limit probe should report the denied child process spawn");
#else
    Expect(result.success, "CLI process-limit probe should stay executable on platforms without enforced process caps");
    Expect(result.stdout_text.find("resource-limit-skip") != std::string::npos,
        "CLI process-limit probe should report the non-Windows skip marker");
#endif
}

void TestCliHostTimeoutKillsProcessTree(const std::filesystem::path& workspace) {
    agentos::CliHost cli_host;
    const auto result = cli_host.run(agentos::CliRunRequest{
        .spec = MakeTimeoutProcessTreeSpec(),
        .workspace_path = workspace,
    });

    Expect(!result.success, "CLI timeout process-tree probe should fail on timeout");
    Expect(result.timed_out, "CLI timeout process-tree probe should surface timed_out=true");
    Expect(result.error_code == "Timeout", "CLI timeout process-tree probe should return Timeout");
    Expect(result.duration_ms < 4000, "CLI timeout process-tree probe should not hang until the child exits naturally");
}

void TestExternalCliSpecLoader(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "external_cli_spec_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "cli_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo external {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"external {{message}}\"";
#endif

    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "cli_specs" / "external_echo.tsv", std::ios::binary);
        spec_file
            << "external_echo" << '\t'
            << "Echo from an external CLI spec." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object","required":["stdout","stderr","exit_code"]})" << '\t'
            << "4096" << '\t'
            << "PATH" << '\t'
            << "268435456" << '\t'
            << "16" << '\t'
            << "10" << '\t'
            << "512"
            << '\n';
    }
    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "cli_specs" / "empty_required_args.tsv", std::ios::binary);
        spec_file
            << "empty_required_args" << '\t'
            << "External CLI spec with no required args." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object","required":["stdout","stderr","exit_code"]})" << '\t'
            << "4096" << '\t'
            << "PATH"
            << '\n';
    }
    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "cli_specs" / "z_duplicate_external_echo.tsv", std::ios::binary);
        spec_file
            << "external_echo" << '\t'
            << "Duplicate external CLI spec fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    agentos::CliHost cli_host;
    const auto loaded_specs = agentos::LoadCliSpecsWithDiagnostics(isolated_workspace / "runtime" / "cli_specs");
    Expect(loaded_specs.diagnostics.size() == 1, "external CLI spec diagnostics should report duplicate fixture specs");
    if (!loaded_specs.diagnostics.empty()) {
        Expect(loaded_specs.diagnostics[0].reason.find("duplicate CLI spec name") != std::string::npos,
            "external CLI duplicate diagnostic should identify duplicate names");
    }
    const auto specs = agentos::LoadCliSpecsFromDirectory(isolated_workspace / "runtime" / "cli_specs");
    Expect(specs.size() == 2, "external CLI spec loader should parse both fixture specs");
    const auto spec_it = std::find_if(specs.begin(), specs.end(), [](const agentos::CliSpec& spec) {
        return spec.name == "external_echo";
    });
    const auto empty_required_args_it = std::find_if(specs.begin(), specs.end(), [](const agentos::CliSpec& spec) {
        return spec.name == "empty_required_args";
    });
    Expect(empty_required_args_it != specs.end(),
        "external CLI spec loader should parse specs with an empty required_args field");
    if (empty_required_args_it != specs.end()) {
        Expect(empty_required_args_it->required_args.empty(),
            "external CLI spec loader should preserve empty required_args as an empty list");
        Expect(empty_required_args_it->parse_mode == "text",
            "external CLI spec loader should not shift fields after an empty required_args column");
    }
    if (spec_it != specs.end()) {
        Expect(spec_it->memory_limit_bytes == 268435456, "external CLI spec should parse memory_limit_bytes");
        Expect(spec_it->max_processes == 16, "external CLI spec should parse max_processes");
        Expect(spec_it->cpu_time_limit_seconds == 10, "external CLI spec should parse cpu_time_limit_seconds");
        Expect(spec_it->file_descriptor_limit == 512, "external CLI spec should parse file_descriptor_limit");
    }
    for (const auto& spec : specs) {
        runtime.skill_registry.register_skill(std::make_shared<agentos::CliSkillInvoker>(spec, cli_host));
    }

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "external-cli-spec",
        .task_type = "external_echo",
        .objective = "execute loaded external cli spec",
        .workspace_path = isolated_workspace,
        .inputs = {
            {"message", "hello"},
        },
    });

    Expect(result.success, "external CLI spec should load and execute as a skill");
    Expect(result.route_target == "external_echo", "external CLI spec should register by its configured name");
    Expect(result.output_json.find("external hello") != std::string::npos, "external CLI spec output should include rendered argument");

    const auto invalid_workspace = workspace / "cli_spec_invalid_numeric_isolated";
    std::filesystem::remove_all(invalid_workspace);
    std::filesystem::create_directories(invalid_workspace / "runtime" / "cli_specs");
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_timeout.tsv", std::ios::binary);
        spec_file
            << "bad_timeout_cli" << '\t'
            << "Invalid timeout fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "not-a-number"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "negative_timeout.tsv", std::ios::binary);
        spec_file
            << "negative_timeout_cli" << '\t'
            << "Negative timeout fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "-1"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "unused_required_arg.tsv", std::ios::binary);
        spec_file
            << "unused_required_arg_cli" << '\t'
            << "Unused required argument fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "missing_placeholder" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_schema.tsv", std::ios::binary);
        spec_file
            << "invalid_schema_cli" << '\t'
            << "Invalid schema fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << "not-json"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_parse_mode.tsv", std::ios::binary);
        spec_file
            << "invalid_parse_mode_cli" << '\t'
            << "Invalid parse mode fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "yaml" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_risk.tsv", std::ios::binary);
        spec_file
            << "invalid_risk_cli" << '\t'
            << "Invalid risk fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "experimental" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_permission.tsv", std::ios::binary);
        spec_file
            << "invalid_permission_cli" << '\t'
            << "Invalid permission fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.launch" << '\t'
            << "3000"
            << '\n';
    }
    const auto invalid_loaded_specs = agentos::LoadCliSpecsWithDiagnostics(invalid_workspace / "runtime" / "cli_specs");
    Expect(invalid_loaded_specs.specs.empty(), "external CLI spec loader should skip invalid numeric fields");
    Expect(invalid_loaded_specs.diagnostics.size() == 7,
        "external CLI spec loader should report invalid numeric, required-arg, schema, parse-mode, risk, and permission fields as diagnostics");
    bool saw_invalid_timeout = false;
    bool saw_negative_timeout = false;
    bool saw_unused_required_arg = false;
    bool saw_invalid_schema = false;
    bool saw_invalid_parse_mode = false;
    bool saw_invalid_risk = false;
    bool saw_invalid_permission = false;
    for (const auto& diagnostic : invalid_loaded_specs.diagnostics) {
        saw_invalid_timeout = saw_invalid_timeout || diagnostic.reason.find("invalid integer field timeout_ms") != std::string::npos;
        saw_negative_timeout = saw_negative_timeout || diagnostic.reason.find("timeout_ms must be >= 1") != std::string::npos;
        saw_unused_required_arg =
            saw_unused_required_arg || diagnostic.reason.find("required_arg is not referenced") != std::string::npos;
        saw_invalid_schema = saw_invalid_schema || diagnostic.reason.find("input_schema_json") != std::string::npos;
        saw_invalid_parse_mode = saw_invalid_parse_mode || diagnostic.reason.find("unsupported CLI parse_mode") != std::string::npos;
        saw_invalid_risk = saw_invalid_risk || diagnostic.reason.find("unsupported risk_level") != std::string::npos;
        saw_invalid_permission = saw_invalid_permission || diagnostic.reason.find("unknown permissions") != std::string::npos;
    }
    Expect(saw_invalid_timeout, "external CLI numeric diagnostic should identify invalid timeout_ms values");
    Expect(saw_negative_timeout, "external CLI numeric diagnostic should reject negative timeout_ms values");
    Expect(saw_unused_required_arg, "external CLI loader should reject unused required_args");
    Expect(saw_invalid_schema, "external CLI loader should reject invalid input_schema_json values");
    Expect(saw_invalid_parse_mode, "external CLI loader should reject unsupported parse modes");
    Expect(saw_invalid_risk, "external CLI loader should reject unsupported risk levels");
    Expect(saw_invalid_permission, "external CLI loader should reject unknown permissions");
}

void TestPluginSpecLoaderAndInvoker(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "plugin_spec_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "powershell";
    const auto args_template = "-NoProfile,-NonInteractive,-Command,Write-Output '{\"message\":\"{{message}}\"}'";
    const auto health_args_template = "-NoProfile,-NonInteractive,-Command,exit 0";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' '{\"message\":\"{{message}}\"}'";
    const auto health_args_template = "-c,exit 0";
#endif

    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "plugin_specs" / "echo_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "echo_plugin" << '\t'
            << "Echo through a plugin host fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object","required":["message"],"properties":{"message":{"type":"string"}}})" << '\t'
            << "4096" << '\t'
            << "PATH" << '\t'
            << "true" << '\t'
            << "268435456" << '\t'
            << "16" << '\t'
            << "10" << '\t'
            << "512" << '\t'
            << health_args_template << '\t'
            << "1000" << '\t'
            << "workspace" << '\t'
            << "oneshot" << '\t'
            << "1200" << '\t'
            << "45000"
            << '\n';
    }
    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "plugin_specs" / "unsupported_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "unsupported_plugin" << '\t'
            << "Unsupported protocol fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "json-rpc-v9" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "plugin_specs" / "unknown_version_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v9" << '\t'
            << "unknown_version_plugin" << '\t'
            << "Unsupported manifest fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "plugin_specs" / "empty_required_args_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "empty_required_args_plugin" << '\t'
            << "Empty required_args fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
#ifdef _WIN32
        const auto json_args_template =
            R"(  "args_template": ["/d", "/s", "/c", "echo {\"\"plugin\"\":\"\"json\"\",\"\"message\"\":\"\"{{message}}\"\"}"],)";
        const auto json_health_args_template = R"(  "health_args_template": ["/d", "/s", "/c", "exit 0"],)";
#else
        const auto json_args_template =
            R"(  "args_template": ["-c", "printf '%s\\n' '{\"plugin\":\"json\",\"message\":\"{{message}}\"}'"],)";
        const auto json_health_args_template = R"(  "health_args_template": ["-c", "exit 0"],)";
#endif
        std::ofstream spec_file(isolated_workspace / "runtime" / "plugin_specs" / "json_echo_plugin.json", std::ios::binary);
        spec_file
            << "{\n"
            << R"(  "manifest_version": "plugin.v1",)" << '\n'
            << R"(  "name": "json_echo_plugin",)" << '\n'
            << R"(  "description": "JSON manifest plugin fixture.",)" << '\n'
            << R"(  "binary": ")" << binary << R"(",)" << '\n'
            << json_args_template << '\n'
            << R"(  "required_args": ["message"],)" << '\n'
            << R"(  "protocol": "json-rpc-v0",)" << '\n'
            << R"(  "input_schema_json": {"type": "object", "required": ["message"]},)" << '\n'
            << R"(  "output_schema_json": {"type": "object"},)" << '\n'
            << R"(  "risk_level": "low",)" << '\n'
            << R"(  "permissions": ["process.spawn"],)" << '\n'
            << R"(  "timeout_ms": 3000,)" << '\n'
            << R"(  "output_limit_bytes": 4096,)" << '\n'
            << R"(  "env_allowlist": ["PATH"],)" << '\n'
            << R"(  "idempotent": true,)" << '\n'
            << R"(  "memory_limit_bytes": 268435456,)" << '\n'
            << R"(  "max_processes": 16,)" << '\n'
            << R"(  "cpu_time_limit_seconds": 10,)" << '\n'
            << R"(  "file_descriptor_limit": 512,)" << '\n'
            << R"(  "sandbox_mode": "workspace",)" << '\n'
            << R"(  "lifecycle_mode": "persistent",)" << '\n'
            << R"(  "startup_timeout_ms": 1500,)" << '\n'
            << R"(  "idle_timeout_ms": 60000,)" << '\n'
            << json_health_args_template << '\n'
            << "}\n";
    }
    {
        std::ofstream spec_file(isolated_workspace / "runtime" / "plugin_specs" / "z_duplicate_echo_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "echo_plugin" << '\t'
            << "Duplicate plugin fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    agentos::CliHost cli_host;
    agentos::PluginHost plugin_host(cli_host);
    const auto loaded_specs = agentos::LoadPluginSpecsWithDiagnostics(isolated_workspace / "runtime" / "plugin_specs");
    Expect(loaded_specs.diagnostics.size() == 3,
        "plugin loader diagnostics should include unsupported manifest/protocol and duplicate specs");
    if (loaded_specs.diagnostics.size() == 3) {
        const bool has_unsupported = std::any_of(
            loaded_specs.diagnostics.begin(),
            loaded_specs.diagnostics.end(),
            [](const agentos::PluginLoadDiagnostic& diagnostic) {
                return diagnostic.reason.find("unsupported") != std::string::npos;
            });
        const bool has_duplicate = std::any_of(
            loaded_specs.diagnostics.begin(),
            loaded_specs.diagnostics.end(),
            [](const agentos::PluginLoadDiagnostic& diagnostic) {
                return diagnostic.reason.find("duplicate plugin spec name") != std::string::npos;
            });
        Expect(has_unsupported,
            "plugin loader diagnostic should explain unsupported specs");
        Expect(has_duplicate,
            "plugin loader diagnostic should explain duplicate specs");
        Expect(loaded_specs.diagnostics[0].line_number == 1,
            "plugin loader diagnostic should include source line number");
    }

    const auto specs = agentos::LoadPluginSpecsFromDirectory(isolated_workspace / "runtime" / "plugin_specs");
    Expect(specs.size() == 3, "plugin loader should load TSV and JSON manifests while skipping unsupported manifest versions and protocols");
    const auto spec_it = std::find_if(specs.begin(), specs.end(), [](const agentos::PluginSpec& spec) {
        return spec.name == "echo_plugin";
    });
    const auto empty_required_args_it = std::find_if(specs.begin(), specs.end(), [](const agentos::PluginSpec& spec) {
        return spec.name == "empty_required_args_plugin";
    });
    const auto json_spec_it = std::find_if(specs.begin(), specs.end(), [](const agentos::PluginSpec& spec) {
        return spec.name == "json_echo_plugin";
    });
    Expect(empty_required_args_it != specs.end(), "plugin loader should parse specs with an empty required_args field");
    if (empty_required_args_it != specs.end()) {
        Expect(empty_required_args_it->required_args.empty(), "plugin loader should preserve empty required_args as an empty list");
        Expect(empty_required_args_it->protocol == "stdio-json-v0",
            "plugin loader should not shift fields after an empty required_args column");
    }
    if (spec_it != specs.end()) {
        const auto& spec = *spec_it;
        Expect(spec.memory_limit_bytes == 268435456, "plugin spec should parse memory_limit_bytes");
        Expect(spec.max_processes == 16, "plugin spec should parse max_processes");
        Expect(spec.cpu_time_limit_seconds == 10, "plugin spec should parse cpu_time_limit_seconds");
        Expect(spec.file_descriptor_limit == 512, "plugin spec should parse file_descriptor_limit");
        Expect(!spec.health_args_template.empty(), "plugin spec should parse health_args_template");
        Expect(spec.health_timeout_ms == 1000, "plugin spec should parse health_timeout_ms");
        Expect(spec.sandbox_mode == "workspace", "plugin spec should parse sandbox_mode");
        Expect(spec.lifecycle_mode == "oneshot", "plugin spec should parse lifecycle_mode");
        Expect(spec.startup_timeout_ms == 1200, "plugin spec should parse startup_timeout_ms");
        Expect(spec.idle_timeout_ms == 45000, "plugin spec should parse idle_timeout_ms");
        const auto health = agentos::CheckPluginHealth(spec);
        Expect(health.supported, "plugin health should mark a loaded spec as supported");
        Expect(health.healthy, "plugin health should mark an available fixture binary as healthy");
        Expect(health.reason == "ok", "plugin health should explain a healthy plugin with ok");
        const auto probe_health = agentos::CheckPluginHealth(spec, cli_host, isolated_workspace);
        Expect(probe_health.healthy, "plugin health should run a successful declared health probe");
        Expect(probe_health.reason == "ok", "plugin health probe should explain a healthy plugin with ok");
    }
    Expect(json_spec_it != specs.end(), "plugin loader should parse JSON plugin manifests");
    if (json_spec_it != specs.end()) {
        const auto& spec = *json_spec_it;
        Expect(spec.source_file.filename() == "json_echo_plugin.json", "JSON plugin spec should keep its source file");
        Expect(spec.source_line_number == 1, "JSON plugin spec should use line 1 as its source line");
        Expect(spec.input_schema_json.find(R"("required")") != std::string::npos,
            "JSON plugin spec should preserve object-valued input_schema_json");
        Expect(!spec.args_template.empty(), "JSON plugin spec should parse args_template string arrays");
        Expect(std::any_of(spec.args_template.begin(), spec.args_template.end(), [](const std::string& arg) {
                   return arg.find("{{message}}") != std::string::npos;
               }),
            "JSON plugin spec should preserve placeholders in args_template string arrays");
        Expect(spec.required_args.size() == 1 && spec.required_args[0] == "message",
            "JSON plugin spec should parse required_args string arrays");
        Expect(spec.permissions.size() == 1 && spec.permissions[0] == "process.spawn",
            "JSON plugin spec should parse permissions string arrays");
        Expect(spec.env_allowlist.size() == 1 && spec.env_allowlist[0] == "PATH",
            "JSON plugin spec should parse env_allowlist string arrays");
        Expect(spec.memory_limit_bytes == 268435456, "JSON plugin spec should parse memory_limit_bytes");
        Expect(spec.max_processes == 16, "JSON plugin spec should parse max_processes");
        Expect(spec.cpu_time_limit_seconds == 10, "JSON plugin spec should parse cpu_time_limit_seconds");
        Expect(spec.file_descriptor_limit == 512, "JSON plugin spec should parse file_descriptor_limit");
        Expect(!spec.health_args_template.empty(), "JSON plugin spec should parse health_args_template");
        Expect(spec.sandbox_mode == "workspace", "JSON plugin spec should parse sandbox_mode");
        Expect(spec.lifecycle_mode == "persistent", "JSON plugin spec should parse lifecycle_mode");
        Expect(spec.startup_timeout_ms == 1500, "JSON plugin spec should parse startup_timeout_ms");
        Expect(spec.idle_timeout_ms == 60000, "JSON plugin spec should parse idle_timeout_ms");
        Expect(spec.protocol == "json-rpc-v0", "persistent JSON plugin spec should use json-rpc-v0");
    }
    for (const auto& spec : specs) {
        runtime.skill_registry.register_skill(std::make_shared<agentos::PluginSkillInvoker>(spec, plugin_host));
    }

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "plugin-spec",
        .task_type = "echo_plugin",
        .objective = "execute loaded plugin spec",
        .workspace_path = isolated_workspace,
        .inputs = {
            {"message", "hello"},
        },
    });

    Expect(result.success, "plugin spec should load and execute as a skill");
    Expect(result.route_target == "echo_plugin", "plugin spec should register by its configured name");
    Expect(result.output_json.find("plugin.v1") != std::string::npos, "plugin output should include manifest version");
    Expect(result.output_json.find("stdio-json-v0") != std::string::npos, "plugin output should include protocol");
    Expect(result.output_json.find(R"("lifecycle_mode":"oneshot")") != std::string::npos,
        "plugin output should include lifecycle mode");
    Expect(result.output_json.find(R"("lifecycle_event":"oneshot")") != std::string::npos,
        "plugin output should include lifecycle event");
    Expect(result.output_json.find("hello") != std::string::npos, "plugin output should include rendered argument");
    Expect(result.output_json.find(R"("plugin_output":{)") != std::string::npos,
        "plugin output should include structured plugin_output JSON");
    if (spec_it != specs.end()) {
        auto sandbox_spec = *spec_it;
#ifdef _WIN32
        sandbox_spec.args_template = {
            "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"message\":\"sandbox\"}'"};
#else
        sandbox_spec.args_template = {"-c", "printf '%s\\n' '{\"message\":\"sandbox\"}'"};
#endif
        sandbox_spec.required_args = {"target_path"};
        const auto sandbox_denied = plugin_host.run(agentos::PluginRunRequest{
            .spec = sandbox_spec,
            .arguments = {
                {"target_path", "../outside.txt"},
            },
            .workspace_path = isolated_workspace,
        });
        Expect(!sandbox_denied.success, "plugin workspace sandbox should deny path arguments outside the workspace");
        Expect(sandbox_denied.error_code == "PluginSandboxDenied",
            "plugin workspace sandbox denial should use a stable error code");
        sandbox_spec.sandbox_mode = "none";
        const auto sandbox_disabled = plugin_host.run(agentos::PluginRunRequest{
            .spec = sandbox_spec,
            .arguments = {
                {"target_path", "../outside.txt"},
            },
            .workspace_path = isolated_workspace,
        });
        Expect(sandbox_disabled.success, "plugin sandbox_mode=none should allow host-managed path arguments");
    }

#ifdef _WIN32
    const std::vector<std::string> json_rpc_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"jsonrpc\":\"2.0\",\"id\":\"agentos-plugin\",\"result\":{\"message\":\"json-rpc-ok\"}}'"};
    const std::vector<std::string> json_rpc_invalid_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"jsonrpc\":\"2.0\",\"id\":\"agentos-plugin\",\"error\":{\"code\":-32000,\"message\":\"failed\"}}'"};
#else
    const std::vector<std::string> json_rpc_args = {
        "-c", "printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":\"agentos-plugin\",\"result\":{\"message\":\"json-rpc-ok\"}}'"};
    const std::vector<std::string> json_rpc_invalid_args = {
        "-c", "printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":\"agentos-plugin\",\"error\":{\"code\":-32000,\"message\":\"failed\"}}'"};
#endif
    agentos::PluginSpec json_rpc_spec{
        .manifest_version = "plugin.v1",
        .name = "json_rpc_plugin",
        .description = "JSON-RPC plugin response fixture.",
        .binary = binary,
        .args_template = json_rpc_args,
        .required_args = {},
        .protocol = "json-rpc-v0",
        .output_schema_json = R"({"type":"object","required":["message"],"properties":{"message":{"type":"string"}}})",
        .permissions = {"process.spawn"},
    };
    const auto json_rpc_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(json_rpc_run.success, "plugin host should accept json-rpc-v0 response objects");
    agentos::PluginSkillInvoker json_rpc_invoker(json_rpc_spec, plugin_host);
    const auto json_rpc_skill = json_rpc_invoker.execute(agentos::SkillCall{
        .skill_name = "json_rpc_plugin",
        .workspace_id = isolated_workspace.string(),
    });
    Expect(json_rpc_skill.success, "plugin invoker should execute json-rpc-v0 plugins");
    Expect(json_rpc_skill.json_output.find(R"("protocol":"json-rpc-v0")") != std::string::npos,
        "json-rpc plugin output should preserve protocol metadata");
    Expect(json_rpc_skill.json_output.find(R"("plugin_output":{"message":"json-rpc-ok"})") != std::string::npos,
        "json-rpc plugin output should embed the JSON-RPC result object");

#ifdef _WIN32
    const auto persistent_script = isolated_workspace / "persistent_json_rpc.ps1";
    {
        std::ofstream script(persistent_script, std::ios::binary);
        script
            << "$counter = 0\n"
            << "while (($line = [Console]::In.ReadLine()) -ne $null) {\n"
            << "  $counter += 1\n"
            << "  Write-Output \"{\"\"jsonrpc\"\":\"\"2.0\"\",\"\"id\"\":$counter,\"\"result\"\":{\"\"message\"\":\"\"persistent-$counter\"\"}}\"\n"
            << "  [Console]::Out.Flush()\n"
            << "}\n";
    }
#else
    const auto persistent_script = isolated_workspace / "persistent_json_rpc.sh";
    {
        std::ofstream script(persistent_script, std::ios::binary);
        script
            << "#!/usr/bin/env sh\n"
            << "counter=0\n"
            << "while IFS= read -r line; do\n"
            << "  counter=$((counter + 1))\n"
            << "  printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"message\":\"persistent-%s\"}}\\n' \"$counter\" \"$counter\"\n"
            << "done\n";
    }
    std::filesystem::permissions(
        persistent_script,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
    agentos::PluginSpec persistent_json_rpc_spec = json_rpc_spec;
    persistent_json_rpc_spec.name = "persistent_json_rpc";
    persistent_json_rpc_spec.lifecycle_mode = "persistent";
    persistent_json_rpc_spec.timeout_ms = 1000;
#ifdef _WIN32
    persistent_json_rpc_spec.args_template = {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", persistent_script.string()};
#else
    persistent_json_rpc_spec.args_template = {persistent_script.string()};
#endif
    const auto persistent_health = agentos::CheckPluginHealth(persistent_json_rpc_spec, cli_host, isolated_workspace);
    Expect(persistent_health.healthy, "persistent plugin health should accept a supported persistent json-rpc plugin");
    const auto persistent_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(persistent_run.success, "persistent plugin should process a JSON-RPC request through a long-running session");
    Expect(persistent_run.stdout_text.find("persistent-1") != std::string::npos,
        "persistent plugin should return the first session response");
    Expect(persistent_run.lifecycle_event == "started",
        "persistent plugin first request should report session start");
    Expect(plugin_host.active_session_count() == 1,
        "plugin host should report active persistent sessions");
    const auto persistent_second_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(persistent_second_run.success, "persistent plugin should reuse the session for a second request");
    Expect(persistent_second_run.stdout_text.find("persistent-2") != std::string::npos,
        "persistent plugin should keep process state across requests");
    Expect(persistent_second_run.lifecycle_event == "reused",
        "persistent plugin second request should report session reuse");
    Expect(plugin_host.close_all_sessions() >= 1,
        "plugin host should close active persistent sessions on demand");
    Expect(plugin_host.active_session_count() == 0,
        "plugin host should report zero active sessions after manual close");
    const auto persistent_after_close_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(persistent_after_close_run.success,
        "persistent plugin should start a new session after manual close");
    Expect(persistent_after_close_run.lifecycle_event == "started",
        "persistent plugin should report started after manual session close");
    Expect(persistent_after_close_run.stdout_text.find("persistent-1") != std::string::npos,
        "persistent plugin process state should reset after manual session close");
    persistent_json_rpc_spec.idle_timeout_ms = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto persistent_idle_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(persistent_idle_run.success,
        "persistent plugin should restart after idle timeout");
    Expect(persistent_idle_run.lifecycle_event == "idle_restarted",
        "persistent plugin should report idle restart");
    Expect(persistent_idle_run.stdout_text.find("persistent-1") != std::string::npos,
        "persistent plugin process state should reset after idle restart");
    persistent_json_rpc_spec.idle_timeout_ms = 30000;

    agentos::PluginHost limited_pool_plugin_host(
        cli_host,
        agentos::PluginHostOptions{.max_persistent_sessions = 1});
    const auto limited_first_run = limited_pool_plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(limited_first_run.success,
        "persistent plugin pool should execute the first session");
    Expect(limited_pool_plugin_host.active_session_count() == 1,
        "persistent plugin pool should keep the first session active");
    agentos::PluginSpec second_persistent_json_rpc_spec = persistent_json_rpc_spec;
    second_persistent_json_rpc_spec.name = "persistent_json_rpc_second";
    const auto limited_second_run = limited_pool_plugin_host.run(agentos::PluginRunRequest{
        .spec = second_persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(limited_second_run.success,
        "persistent plugin pool should execute a second distinct session");
    Expect(limited_pool_plugin_host.active_session_count() == 1,
        "persistent plugin pool should evict the oldest session when over capacity");
    const auto limited_first_after_eviction_run = limited_pool_plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_json_rpc_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(limited_first_after_eviction_run.success,
        "persistent plugin pool should restart an evicted session on demand");
    Expect(limited_first_after_eviction_run.lifecycle_event == "started",
        "persistent plugin pool should report a fresh start after LRU eviction");
    Expect(limited_first_after_eviction_run.stdout_text.find("persistent-1") != std::string::npos,
        "persistent plugin pool should reset process state after LRU eviction");
    Expect(limited_pool_plugin_host.close_all_sessions() == 1,
        "persistent plugin pool should close the remaining capped session");

#ifdef _WIN32
    const auto persistent_crash_script = isolated_workspace / "persistent_crash_once.ps1";
    {
        std::ofstream script(persistent_crash_script, std::ios::binary);
        script
            << "$state = Join-Path (Get-Location) 'persistent_crash_state.txt'\n"
            << "$count = 0\n"
            << "if (Test-Path $state) { $count = [int](Get-Content $state) }\n"
            << "$count += 1\n"
            << "Set-Content -Path $state -Value $count\n"
            << "$line = [Console]::In.ReadLine()\n"
            << "Write-Output \"{\"\"jsonrpc\"\":\"\"2.0\"\",\"\"id\"\":1,\"\"result\"\":{\"\"message\"\":\"\"restart-$count\"\"}}\"\n"
            << "[Console]::Out.Flush()\n"
            << "exit 0\n";
    }
#else
    const auto persistent_crash_script = isolated_workspace / "persistent_crash_once.sh";
    {
        std::ofstream script(persistent_crash_script, std::ios::binary);
        script
            << "#!/usr/bin/env sh\n"
            << "state=persistent_crash_state.txt\n"
            << "count=0\n"
            << "if [ -f \"$state\" ]; then count=$(cat \"$state\"); fi\n"
            << "count=$((count + 1))\n"
            << "printf '%s\\n' \"$count\" > \"$state\"\n"
            << "IFS= read -r line || exit 0\n"
            << "printf '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"message\":\"restart-%s\"}}\\n' \"$count\"\n"
            << "exit 0\n";
    }
    std::filesystem::permissions(
        persistent_crash_script,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
    agentos::PluginSpec persistent_crash_spec = persistent_json_rpc_spec;
    persistent_crash_spec.name = "persistent_crash_json_rpc";
#ifdef _WIN32
    persistent_crash_spec.args_template = {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", persistent_crash_script.string()};
#else
    persistent_crash_spec.args_template = {persistent_crash_script.string()};
#endif
    const auto persistent_crash_first = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_crash_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(persistent_crash_first.success, "persistent plugin should return a response before crashing");
    Expect(persistent_crash_first.lifecycle_event == "started",
        "persistent crash fixture first request should report started");
    const auto persistent_crash_second = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_crash_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(persistent_crash_second.success, "persistent plugin should restart after a crashed session");
    Expect(persistent_crash_second.stdout_text.find("restart-2") != std::string::npos,
        "persistent plugin restart should launch a new process after crash");
    Expect(persistent_crash_second.lifecycle_event == "restarted",
        "persistent plugin restart should report restarted lifecycle event");

#ifdef _WIN32
    const auto persistent_timeout_script = isolated_workspace / "persistent_timeout.ps1";
    {
        std::ofstream script(persistent_timeout_script, std::ios::binary);
        script
            << "while (($line = [Console]::In.ReadLine()) -ne $null) {\n"
            << "  Start-Sleep -Milliseconds 250\n"
            << "  Write-Output '{\"\"jsonrpc\"\":\"\"2.0\"\",\"\"id\"\":1,\"\"result\"\":{\"\"message\"\":\"\"late\"\"}}'\n"
            << "  [Console]::Out.Flush()\n"
            << "}\n";
    }
    const auto persistent_malformed_script = isolated_workspace / "persistent_malformed.ps1";
    {
        std::ofstream script(persistent_malformed_script, std::ios::binary);
        script
            << "while (($line = [Console]::In.ReadLine()) -ne $null) {\n"
            << "  Write-Output 'not-json-rpc'\n"
            << "  [Console]::Out.Flush()\n"
            << "}\n";
    }
#else
    const auto persistent_timeout_script = isolated_workspace / "persistent_timeout.sh";
    {
        std::ofstream script(persistent_timeout_script, std::ios::binary);
        script
            << "#!/usr/bin/env sh\n"
            << "while IFS= read -r line; do\n"
            << "  sleep 1\n"
            << "  printf '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"message\":\"late\"}}\\n'\n"
            << "done\n";
    }
    const auto persistent_malformed_script = isolated_workspace / "persistent_malformed.sh";
    {
        std::ofstream script(persistent_malformed_script, std::ios::binary);
        script
            << "#!/usr/bin/env sh\n"
            << "while IFS= read -r line; do printf '%s\\n' 'not-json-rpc'; done\n";
    }
    std::filesystem::permissions(
        persistent_timeout_script,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
    std::filesystem::permissions(
        persistent_malformed_script,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
    agentos::PluginSpec persistent_timeout_spec = persistent_json_rpc_spec;
    persistent_timeout_spec.name = "persistent_timeout_json_rpc";
    persistent_timeout_spec.timeout_ms = 50;
#ifdef _WIN32
    persistent_timeout_spec.args_template = {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", persistent_timeout_script.string()};
#else
    persistent_timeout_spec.args_template = {persistent_timeout_script.string()};
#endif
    const auto persistent_timeout_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_timeout_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!persistent_timeout_run.success, "persistent plugin should fail timed-out JSON-RPC requests");
    Expect(persistent_timeout_run.error_code == "Timeout", "persistent plugin timeout should use the Timeout error code");

    agentos::PluginSpec persistent_malformed_spec = persistent_json_rpc_spec;
    persistent_malformed_spec.name = "persistent_malformed_json_rpc";
#ifdef _WIN32
    persistent_malformed_spec.args_template = {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", persistent_malformed_script.string()};
#else
    persistent_malformed_spec.args_template = {persistent_malformed_script.string()};
#endif
    const auto persistent_malformed_health =
        agentos::CheckPluginHealth(persistent_malformed_spec, cli_host, isolated_workspace);
    Expect(!persistent_malformed_health.healthy,
        "persistent plugin health should reject malformed JSON-RPC round-trip responses");
    const auto persistent_malformed_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = persistent_malformed_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!persistent_malformed_run.success, "persistent plugin should reject malformed JSON-RPC responses");
    Expect(persistent_malformed_run.error_code == "InvalidPluginOutput",
        "persistent malformed JSON-RPC responses should use InvalidPluginOutput");

    agentos::PluginSpec json_rpc_invalid_spec = json_rpc_spec;
    json_rpc_invalid_spec.name = "json_rpc_invalid_plugin";
    json_rpc_invalid_spec.args_template = json_rpc_invalid_args;
    const auto json_rpc_invalid_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = json_rpc_invalid_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!json_rpc_invalid_run.success, "plugin host should reject json-rpc-v0 error responses");
    Expect(json_rpc_invalid_run.error_code == "InvalidPluginOutput",
        "json-rpc error responses should surface as InvalidPluginOutput");

#ifdef _WIN32
    const std::vector<std::string> invalid_output_args = {"-NoProfile", "-NonInteractive", "-Command", "Write-Output 'not-json'"};
#else
    const std::vector<std::string> invalid_output_args = {"-c", "printf '%s\\n' 'not-json'"};
#endif
    agentos::PluginSpec invalid_output_spec{
        .manifest_version = "plugin.v1",
        .name = "invalid_output_plugin",
        .description = "Invalid plugin stdout fixture.",
        .binary = binary,
        .args_template = invalid_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .permissions = {"process.spawn"},
    };
    const auto invalid_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = invalid_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!invalid_output_run.success, "plugin host should reject successful stdio-json-v0 runs with non-JSON stdout");
    Expect(invalid_output_run.error_code == "InvalidPluginOutput",
        "plugin host should return InvalidPluginOutput for non-JSON stdout");
    Expect(invalid_output_run.error_message.find("stdout must be a JSON object") != std::string::npos,
        "plugin host should explain stdio-json-v0 output shape failures");
    agentos::PluginSkillInvoker invalid_output_invoker(invalid_output_spec, plugin_host);
    const auto invalid_output_skill = invalid_output_invoker.execute(agentos::SkillCall{
        .skill_name = "invalid_output_plugin",
        .workspace_id = isolated_workspace.string(),
    });
    Expect(!invalid_output_skill.success, "plugin invoker should surface invalid output failures");
    Expect(invalid_output_skill.json_output.find(R"("plugin_output":null)") != std::string::npos,
        "plugin invoker should not embed invalid stdout as structured plugin_output");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"other\":\"hello\"}'"};
#else
    const std::vector<std::string> schema_invalid_output_args = {"-c", "printf '%s\\n' '{\"other\":\"hello\"}'"};
#endif
    agentos::PluginSpec schema_invalid_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_output_plugin",
        .description = "Schema-invalid plugin stdout fixture.",
        .binary = binary,
        .args_template = schema_invalid_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","required":["message"]})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_output_run.success,
        "plugin host should reject stdio-json-v0 output missing required output schema fields");
    Expect(schema_invalid_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output schema failures");
    Expect(schema_invalid_output_run.error_message.find("missing required field: message") != std::string::npos,
        "plugin host should explain missing output schema fields");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_type_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"message\":42}'"};
#else
    const std::vector<std::string> schema_invalid_type_output_args = {"-c", "printf '%s\\n' '{\"message\":42}'"};
#endif
    agentos::PluginSpec schema_invalid_type_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_type_output_plugin",
        .description = "Schema-invalid plugin output type fixture.",
        .binary = binary,
        .args_template = schema_invalid_type_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"message":{"type":"string"}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_type_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_type_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_type_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid output schema field types");
    Expect(schema_invalid_type_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output type failures");
    Expect(schema_invalid_type_output_run.error_message.find("invalid type: message:string") != std::string::npos,
        "plugin host should explain invalid output schema field types");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_const_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"mode\":\"loose\"}'"};
#else
    const std::vector<std::string> schema_invalid_const_output_args = {"-c", "printf '%s\\n' '{\"mode\":\"loose\"}'"};
#endif
    agentos::PluginSpec schema_invalid_const_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_const_output_plugin",
        .description = "Schema-invalid plugin output const fixture.",
        .binary = binary,
        .args_template = schema_invalid_const_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"mode":{"type":"string","const":"strict"}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_const_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_const_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_const_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid const constraints");
    Expect(schema_invalid_const_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output const failures");
    Expect(schema_invalid_const_output_run.error_message.find("invalid constraint: mode:const") != std::string::npos,
        "plugin host should explain invalid output const constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_enum_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"status\":\"bad\"}'"};
#else
    const std::vector<std::string> schema_invalid_enum_output_args = {"-c", "printf '%s\\n' '{\"status\":\"bad\"}'"};
#endif
    agentos::PluginSpec schema_invalid_enum_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_enum_output_plugin",
        .description = "Schema-invalid plugin output enum fixture.",
        .binary = binary,
        .args_template = schema_invalid_enum_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"status":{"type":"string","enum":["ok","fine"]}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_enum_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_enum_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_enum_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid enum constraints");
    Expect(schema_invalid_enum_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output enum failures");
    Expect(schema_invalid_enum_output_run.error_message.find("invalid constraint: status:enum") != std::string::npos,
        "plugin host should explain invalid output enum constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_min_length_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"label\":\"x\"}'"};
#else
    const std::vector<std::string> schema_invalid_min_length_output_args = {"-c", "printf '%s\\n' '{\"label\":\"x\"}'"};
#endif
    agentos::PluginSpec schema_invalid_min_length_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_min_length_output_plugin",
        .description = "Schema-invalid plugin output minLength fixture.",
        .binary = binary,
        .args_template = schema_invalid_min_length_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"label":{"type":"string","minLength":2}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_min_length_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_min_length_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_min_length_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid minLength constraints");
    Expect(schema_invalid_min_length_output_run.error_message.find("invalid constraint: label:minLength") != std::string::npos,
        "plugin host should explain invalid output minLength constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_max_length_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"label\":\"toolong\"}'"};
#else
    const std::vector<std::string> schema_invalid_max_length_output_args = {"-c", "printf '%s\\n' '{\"label\":\"toolong\"}'"};
#endif
    agentos::PluginSpec schema_invalid_max_length_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_max_length_output_plugin",
        .description = "Schema-invalid plugin output maxLength fixture.",
        .binary = binary,
        .args_template = schema_invalid_max_length_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"label":{"type":"string","maxLength":4}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_max_length_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_max_length_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_max_length_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid maxLength constraints");
    Expect(schema_invalid_max_length_output_run.error_message.find("invalid constraint: label:maxLength") != std::string::npos,
        "plugin host should explain invalid output maxLength constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_pattern_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"code\":\"ab12\"}'"};
#else
    const std::vector<std::string> schema_invalid_pattern_output_args = {"-c", "printf '%s\\n' '{\"code\":\"ab12\"}'"};
#endif
    agentos::PluginSpec schema_invalid_pattern_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_pattern_output_plugin",
        .description = "Schema-invalid plugin output pattern fixture.",
        .binary = binary,
        .args_template = schema_invalid_pattern_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"code":{"type":"string","pattern":"^[A-Z]{2}[0-9]{2}$"}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_pattern_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_pattern_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_pattern_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid pattern constraints");
    Expect(schema_invalid_pattern_output_run.error_message.find("invalid constraint: code:pattern") != std::string::npos,
        "plugin host should explain invalid output pattern constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_minimum_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"score\":0}'"};
#else
    const std::vector<std::string> schema_invalid_minimum_output_args = {"-c", "printf '%s\\n' '{\"score\":0}'"};
#endif
    agentos::PluginSpec schema_invalid_minimum_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_minimum_output_plugin",
        .description = "Schema-invalid plugin output minimum fixture.",
        .binary = binary,
        .args_template = schema_invalid_minimum_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"score":{"type":"number","minimum":1}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_minimum_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_minimum_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_minimum_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid minimum constraints");
    Expect(schema_invalid_minimum_output_run.error_message.find("invalid constraint: score:minimum") != std::string::npos,
        "plugin host should explain invalid output minimum constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_exclusive_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"score\":10}'"};
#else
    const std::vector<std::string> schema_invalid_exclusive_output_args = {"-c", "printf '%s\\n' '{\"score\":10}'"};
#endif
    agentos::PluginSpec schema_invalid_exclusive_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_exclusive_output_plugin",
        .description = "Schema-invalid plugin output exclusiveMaximum fixture.",
        .binary = binary,
        .args_template = schema_invalid_exclusive_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"score":{"type":"number","exclusiveMaximum":10}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_exclusive_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_exclusive_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_exclusive_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid exclusiveMaximum constraints");
    Expect(schema_invalid_exclusive_output_run.error_message.find("invalid constraint: score:exclusiveMaximum") != std::string::npos,
        "plugin host should explain invalid output exclusiveMaximum constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_multiple_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"score\":3}'"};
#else
    const std::vector<std::string> schema_invalid_multiple_output_args = {"-c", "printf '%s\\n' '{\"score\":3}'"};
#endif
    agentos::PluginSpec schema_invalid_multiple_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_multiple_output_plugin",
        .description = "Schema-invalid plugin output multipleOf fixture.",
        .binary = binary,
        .args_template = schema_invalid_multiple_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json = R"({"type":"object","properties":{"score":{"type":"number","multipleOf":2}}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_multiple_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_multiple_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_multiple_output_run.success,
        "plugin host should reject stdio-json-v0 output with invalid multipleOf constraints");
    Expect(schema_invalid_multiple_output_run.error_message.find("invalid constraint: score:multipleOf") != std::string::npos,
        "plugin host should explain invalid output multipleOf constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_additional_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"message\":\"hello\",\"extra\":\"nope\"}'"};
#else
    const std::vector<std::string> schema_invalid_additional_output_args = {
        "-c", "printf '%s\\n' '{\"message\":\"hello\",\"extra\":\"nope\"}'"};
#endif
    agentos::PluginSpec schema_invalid_additional_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_invalid_additional_output_plugin",
        .description = "Schema-invalid plugin output additionalProperties fixture.",
        .binary = binary,
        .args_template = schema_invalid_additional_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json =
            R"({"type":"object","properties":{"message":{"type":"string"}},"additionalProperties":false})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_additional_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_additional_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_additional_output_run.success,
        "plugin host should reject stdio-json-v0 output with undeclared fields when additionalProperties is false");
    Expect(schema_invalid_additional_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output additionalProperties failures");
    Expect(schema_invalid_additional_output_run.error_message.find("invalid constraint: extra:additionalProperties") !=
               std::string::npos,
        "plugin host should explain output additionalProperties constraints");

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_any_of_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"api_key\":\"key\",\"workspace\":\"main\",\"project\":\"agentos\"}'"};
    const std::vector<std::string> schema_invalid_one_of_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"email\":\"dev@example.test\",\"api_key\":\"key\",\"oauth_token\":\"token\",\"workspace\":\"main\",\"project\":\"agentos\"}'"};
    const std::vector<std::string> schema_invalid_all_of_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"phone\":\"555-0100\",\"oauth_token\":\"token\",\"workspace\":\"main\"}'"};
    const std::vector<std::string> schema_valid_combinator_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"phone\":\"555-0100\",\"oauth_token\":\"token\",\"workspace\":\"main\",\"project\":\"agentos\"}'"};
#else
    const std::vector<std::string> schema_invalid_any_of_output_args = {
        "-c", "printf '%s\\n' '{\"api_key\":\"key\",\"workspace\":\"main\",\"project\":\"agentos\"}'"};
    const std::vector<std::string> schema_invalid_one_of_output_args = {
        "-c",
        "printf '%s\\n' '{\"email\":\"dev@example.test\",\"api_key\":\"key\",\"oauth_token\":\"token\",\"workspace\":\"main\",\"project\":\"agentos\"}'"};
    const std::vector<std::string> schema_invalid_all_of_output_args = {
        "-c", "printf '%s\\n' '{\"phone\":\"555-0100\",\"oauth_token\":\"token\",\"workspace\":\"main\"}'"};
    const std::vector<std::string> schema_valid_combinator_output_args = {
        "-c",
        "printf '%s\\n' '{\"phone\":\"555-0100\",\"oauth_token\":\"token\",\"workspace\":\"main\",\"project\":\"agentos\"}'"};
#endif
    agentos::PluginSpec schema_combinator_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_combinator_output_plugin",
        .description = "Schema plugin output combinator fixture.",
        .binary = binary,
        .args_template = schema_invalid_any_of_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json =
            R"({"type":"object","anyOf":[{"required":["email"]},{"required":["phone"]}],"oneOf":[{"required":["api_key"]},{"required":["oauth_token"]}],"allOf":[{"required":["workspace"]},{"required":["project"]}]})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_any_of_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_combinator_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_any_of_output_run.success,
        "plugin host should reject output matching no anyOf required branch");
    Expect(schema_invalid_any_of_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output anyOf failures");
    Expect(schema_invalid_any_of_output_run.error_message.find("failed anyOf required branches") != std::string::npos,
        "plugin host should explain output anyOf required branch failures: " +
            schema_invalid_any_of_output_run.error_message);

    auto schema_invalid_one_of_output_spec = schema_combinator_output_spec;
    schema_invalid_one_of_output_spec.name = "schema_invalid_one_of_output_plugin";
    schema_invalid_one_of_output_spec.args_template = schema_invalid_one_of_output_args;
    const auto schema_invalid_one_of_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_one_of_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_one_of_output_run.success,
        "plugin host should reject output matching multiple oneOf required branches");
    Expect(schema_invalid_one_of_output_run.error_message.find("failed oneOf required branches") != std::string::npos,
        "plugin host should explain output oneOf required branch failures");

    auto schema_invalid_all_of_output_spec = schema_combinator_output_spec;
    schema_invalid_all_of_output_spec.name = "schema_invalid_all_of_output_plugin";
    schema_invalid_all_of_output_spec.args_template = schema_invalid_all_of_output_args;
    const auto schema_invalid_all_of_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_all_of_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_all_of_output_run.success,
        "plugin host should reject output missing an allOf required branch");
    Expect(schema_invalid_all_of_output_run.error_message.find("failed allOf required branches") != std::string::npos,
        "plugin host should explain output allOf required branch failures: " +
            schema_invalid_all_of_output_run.error_message);

    auto schema_valid_combinator_output_spec = schema_combinator_output_spec;
    schema_valid_combinator_output_spec.name = "schema_valid_combinator_output_plugin";
    schema_valid_combinator_output_spec.args_template = schema_valid_combinator_output_args;
    const auto schema_valid_combinator_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_valid_combinator_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(schema_valid_combinator_output_run.success,
        "plugin host should accept output satisfying all combinator required branches: " +
            schema_valid_combinator_output_run.error_message);

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_min_properties_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"first\":\"a\"}'"};
    const std::vector<std::string> schema_invalid_max_properties_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"first\":\"a\",\"second\":\"b\",\"third\":\"c\"}'"};
    const std::vector<std::string> schema_invalid_property_names_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"BadName\":\"a\"}'"};
    const std::vector<std::string> schema_valid_object_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"first_name\":\"a\",\"second_name\":\"b\"}'"};
#else
    const std::vector<std::string> schema_invalid_min_properties_output_args = {
        "-c", "printf '%s\\n' '{\"first\":\"a\"}'"};
    const std::vector<std::string> schema_invalid_max_properties_output_args = {
        "-c", "printf '%s\\n' '{\"first\":\"a\",\"second\":\"b\",\"third\":\"c\"}'"};
    const std::vector<std::string> schema_invalid_property_names_output_args = {
        "-c", "printf '%s\\n' '{\"BadName\":\"a\"}'"};
    const std::vector<std::string> schema_valid_object_output_args = {
        "-c", "printf '%s\\n' '{\"first_name\":\"a\",\"second_name\":\"b\"}'"};
#endif
    agentos::PluginSpec schema_object_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_object_output_plugin",
        .description = "Schema plugin output object-level fixture.",
        .binary = binary,
        .args_template = schema_invalid_min_properties_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json =
            R"({"type":"object","minProperties":2,"maxProperties":2,"propertyNames":{"pattern":"^[a-z_]+$","minLength":2}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_min_properties_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_object_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_min_properties_output_run.success,
        "plugin host should reject output with too few properties");
    Expect(schema_invalid_min_properties_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output minProperties failures");
    Expect(schema_invalid_min_properties_output_run.error_message.find("failed minProperties") != std::string::npos,
        "plugin host should explain output minProperties failures");

    auto schema_invalid_max_properties_output_spec = schema_object_output_spec;
    schema_invalid_max_properties_output_spec.name = "schema_invalid_max_properties_output_plugin";
    schema_invalid_max_properties_output_spec.args_template = schema_invalid_max_properties_output_args;
    const auto schema_invalid_max_properties_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_max_properties_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_max_properties_output_run.success,
        "plugin host should reject output with too many properties");
    Expect(schema_invalid_max_properties_output_run.error_message.find("failed maxProperties") != std::string::npos,
        "plugin host should explain output maxProperties failures");

    auto schema_invalid_property_names_output_spec = schema_object_output_spec;
    schema_invalid_property_names_output_spec.name = "schema_invalid_property_names_output_plugin";
    schema_invalid_property_names_output_spec.args_template = schema_invalid_property_names_output_args;
    schema_invalid_property_names_output_spec.output_schema_json =
        R"({"type":"object","propertyNames":{"pattern":"^[a-z_]+$","minLength":2}})";
    const auto schema_invalid_property_names_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_property_names_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_property_names_output_run.success,
        "plugin host should reject output with invalid propertyNames");
    Expect(schema_invalid_property_names_output_run.error_message.find("BadName:propertyNames:pattern") !=
               std::string::npos,
        "plugin host should explain output propertyNames failures");

    auto schema_valid_object_output_spec = schema_object_output_spec;
    schema_valid_object_output_spec.name = "schema_valid_object_output_plugin";
    schema_valid_object_output_spec.args_template = schema_valid_object_output_args;
    const auto schema_valid_object_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_valid_object_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(schema_valid_object_output_run.success,
        "plugin host should accept output satisfying object-level constraints: " +
            schema_valid_object_output_run.error_message);

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_dependent_required_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"token\":\"abc\"}'"};
    const std::vector<std::string> schema_invalid_dependencies_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"receipt\":\"r-1\"}'"};
    const std::vector<std::string> schema_invalid_not_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"token\":\"abc\",\"signature\":\"sig\",\"anonymous\":\"true\"}'"};
    const std::vector<std::string> schema_valid_dependency_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"token\":\"abc\",\"signature\":\"sig\",\"receipt\":\"r-1\",\"mode\":\"strict\"}'"};
#else
    const std::vector<std::string> schema_invalid_dependent_required_output_args = {
        "-c", "printf '%s\\n' '{\"token\":\"abc\"}'"};
    const std::vector<std::string> schema_invalid_dependencies_output_args = {
        "-c", "printf '%s\\n' '{\"receipt\":\"r-1\"}'"};
    const std::vector<std::string> schema_invalid_not_output_args = {
        "-c", "printf '%s\\n' '{\"token\":\"abc\",\"signature\":\"sig\",\"anonymous\":\"true\"}'"};
    const std::vector<std::string> schema_valid_dependency_output_args = {
        "-c", "printf '%s\\n' '{\"token\":\"abc\",\"signature\":\"sig\",\"receipt\":\"r-1\",\"mode\":\"strict\"}'"};
#endif
    agentos::PluginSpec schema_dependency_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_dependency_output_plugin",
        .description = "Schema plugin output dependency fixture.",
        .binary = binary,
        .args_template = schema_invalid_dependent_required_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json =
            R"({"type":"object","dependentRequired":{"token":["signature"]},"dependencies":{"receipt":["mode"]},"not":{"required":["token","anonymous"]}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_dependent_required_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_dependency_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_dependent_required_output_run.success,
        "plugin host should reject output missing dependentRequired fields");
    Expect(schema_invalid_dependent_required_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output dependentRequired failures");
    Expect(schema_invalid_dependent_required_output_run.error_message.find("token:dependentRequired:signature") !=
               std::string::npos,
        "plugin host should explain output dependentRequired failures");

    auto schema_invalid_dependencies_output_spec = schema_dependency_output_spec;
    schema_invalid_dependencies_output_spec.name = "schema_invalid_dependencies_output_plugin";
    schema_invalid_dependencies_output_spec.args_template = schema_invalid_dependencies_output_args;
    const auto schema_invalid_dependencies_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_dependencies_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_dependencies_output_run.success,
        "plugin host should reject output missing legacy dependencies fields");
    Expect(schema_invalid_dependencies_output_run.error_message.find("receipt:dependentRequired:mode") !=
               std::string::npos,
        "plugin host should explain output dependencies failures");

    auto schema_invalid_not_output_spec = schema_dependency_output_spec;
    schema_invalid_not_output_spec.name = "schema_invalid_not_output_plugin";
    schema_invalid_not_output_spec.args_template = schema_invalid_not_output_args;
    const auto schema_invalid_not_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_not_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_not_output_run.success,
        "plugin host should reject output matching not.required");
    Expect(schema_invalid_not_output_run.error_message.find("failed not.required") != std::string::npos,
        "plugin host should explain output not.required failures");

    auto schema_valid_dependency_output_spec = schema_dependency_output_spec;
    schema_valid_dependency_output_spec.name = "schema_valid_dependency_output_plugin";
    schema_valid_dependency_output_spec.args_template = schema_valid_dependency_output_args;
    const auto schema_valid_dependency_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_valid_dependency_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(schema_valid_dependency_output_run.success,
        "plugin host should accept output satisfying dependency and not.required constraints: " +
            schema_valid_dependency_output_run.error_message);

#ifdef _WIN32
    const std::vector<std::string> schema_invalid_then_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"mode\":\"auth\"}'"};
    const std::vector<std::string> schema_invalid_else_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"mode\":\"guest\"}'"};
    const std::vector<std::string> schema_valid_then_output_args = {
        "-NoProfile", "-NonInteractive", "-Command", "Write-Output '{\"mode\":\"auth\",\"token\":\"abc\"}'"};
    const std::vector<std::string> schema_valid_else_output_args = {
        "-NoProfile", "-NonInteractive", "-Command",
        "Write-Output '{\"mode\":\"guest\",\"anonymous_reason\":\"readonly\"}'"};
#else
    const std::vector<std::string> schema_invalid_then_output_args = {
        "-c", "printf '%s\\n' '{\"mode\":\"auth\"}'"};
    const std::vector<std::string> schema_invalid_else_output_args = {
        "-c", "printf '%s\\n' '{\"mode\":\"guest\"}'"};
    const std::vector<std::string> schema_valid_then_output_args = {
        "-c", "printf '%s\\n' '{\"mode\":\"auth\",\"token\":\"abc\"}'"};
    const std::vector<std::string> schema_valid_else_output_args = {
        "-c", "printf '%s\\n' '{\"mode\":\"guest\",\"anonymous_reason\":\"readonly\"}'"};
#endif
    agentos::PluginSpec schema_conditional_output_spec{
        .manifest_version = "plugin.v1",
        .name = "schema_conditional_output_plugin",
        .description = "Schema plugin output conditional fixture.",
        .binary = binary,
        .args_template = schema_invalid_then_output_args,
        .required_args = std::vector<std::string>{},
        .protocol = "stdio-json-v0",
        .output_schema_json =
            R"({"type":"object","if":{"properties":{"mode":{"const":"auth"}}},"then":{"required":["token"]},"else":{"required":["anonymous_reason"]}})",
        .permissions = {"process.spawn"},
    };
    const auto schema_invalid_then_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_conditional_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_then_output_run.success,
        "plugin host should reject output missing then-required fields");
    Expect(schema_invalid_then_output_run.error_code == "PluginOutputSchemaValidationFailed",
        "plugin host should return PluginOutputSchemaValidationFailed for output then failures");
    Expect(schema_invalid_then_output_run.error_message.find("failed then required field: token") !=
               std::string::npos,
        "plugin host should explain output then required failures");

    auto schema_invalid_else_output_spec = schema_conditional_output_spec;
    schema_invalid_else_output_spec.name = "schema_invalid_else_output_plugin";
    schema_invalid_else_output_spec.args_template = schema_invalid_else_output_args;
    const auto schema_invalid_else_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_invalid_else_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!schema_invalid_else_output_run.success,
        "plugin host should reject output missing else-required fields");
    Expect(schema_invalid_else_output_run.error_message.find("failed else required field: anonymous_reason") !=
               std::string::npos,
        "plugin host should explain output else required failures");

    auto schema_valid_then_output_spec = schema_conditional_output_spec;
    schema_valid_then_output_spec.name = "schema_valid_then_output_plugin";
    schema_valid_then_output_spec.args_template = schema_valid_then_output_args;
    const auto schema_valid_then_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_valid_then_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(schema_valid_then_output_run.success,
        "plugin host should accept output satisfying then-required fields: " +
            schema_valid_then_output_run.error_message);

    auto schema_valid_else_output_spec = schema_conditional_output_spec;
    schema_valid_else_output_spec.name = "schema_valid_else_output_plugin";
    schema_valid_else_output_spec.args_template = schema_valid_else_output_args;
    const auto schema_valid_else_output_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = schema_valid_else_output_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(schema_valid_else_output_run.success,
        "plugin host should accept output satisfying else-required fields: " +
            schema_valid_else_output_run.error_message);

    const auto invalid_numeric_workspace = workspace / "plugin_spec_invalid_numeric_isolated";
    std::filesystem::remove_all(invalid_numeric_workspace);
    std::filesystem::create_directories(invalid_numeric_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_memory.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "bad_memory_plugin" << '\t'
            << "Invalid memory limit fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "4096" << '\t'
            << "PATH" << '\t'
            << "true" << '\t'
            << "not-a-size"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_bool.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "bad_bool_plugin" << '\t'
            << "Invalid boolean fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "4096" << '\t'
            << "PATH" << '\t'
            << "maybe"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "negative_timeout.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "negative_timeout_plugin" << '\t'
            << "Negative timeout fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "-1"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "unused_required_arg.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "unused_required_arg_plugin" << '\t'
            << "Unused required argument fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "missing_placeholder" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_schema.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_schema_plugin" << '\t'
            << "Invalid schema fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << "{"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_risk.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_risk_plugin" << '\t'
            << "Invalid risk fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "experimental" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_permission.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_permission_plugin" << '\t'
            << "Invalid permission fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.launch" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "missing_spawn_permission.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "missing_spawn_permission_plugin" << '\t'
            << "Missing process.spawn permission fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "filesystem.read" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_health_arg.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_health_arg_plugin" << '\t'
            << "Invalid health template fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "probe {{message}}"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_json_health_arg.json", std::ios::binary);
        spec_file
            << "{\n"
            << R"(  "manifest_version": "plugin.v1",)" << '\n'
            << R"(  "name": "invalid_json_health_arg_plugin",)" << '\n'
            << R"(  "description": "Invalid JSON health template fixture.",)" << '\n'
            << R"(  "binary": ")" << binary << R"(",)" << '\n'
            << R"(  "args_template": ["noop {{message}}"],)" << '\n'
            << R"(  "required_args": ["message"],)" << '\n'
            << R"(  "protocol": "stdio-json-v0",)" << '\n'
            << R"(  "risk_level": "low",)" << '\n'
            << R"(  "permissions": ["process.spawn"],)" << '\n'
            << R"(  "health_args_template": ["probe {{message}}"])" << '\n'
            << "}\n";
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_sandbox.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_sandbox_plugin" << '\t'
            << "Invalid sandbox fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "host"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "invalid_lifecycle.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_lifecycle_plugin" << '\t'
            << "Invalid lifecycle fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "json-rpc-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "workspace" << '\t'
            << "daemon"
            << '\n';
    }
    {
        std::ofstream spec_file(invalid_numeric_workspace / "runtime" / "plugin_specs" / "persistent_stdio.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "persistent_stdio_plugin" << '\t'
            << "Persistent stdio fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "workspace" << '\t'
            << "persistent"
            << '\n';
    }
    const auto invalid_numeric_loaded_specs =
        agentos::LoadPluginSpecsWithDiagnostics(invalid_numeric_workspace / "runtime" / "plugin_specs");
    Expect(invalid_numeric_loaded_specs.specs.empty(), "plugin loader should skip invalid fields");
    Expect(invalid_numeric_loaded_specs.diagnostics.size() == 13,
        "plugin loader should report invalid numeric, boolean, required-arg, schema, risk, permission, health, sandbox, and lifecycle fields as diagnostics");
    bool saw_invalid_memory = false;
    bool saw_invalid_bool = false;
    bool saw_negative_timeout = false;
    bool saw_unused_required_arg = false;
    bool saw_invalid_schema = false;
    bool saw_invalid_risk = false;
    bool saw_invalid_permission = false;
    bool saw_missing_spawn_permission = false;
    bool saw_invalid_health_arg = false;
    bool saw_invalid_sandbox = false;
    bool saw_invalid_lifecycle = false;
    bool saw_persistent_stdio = false;
    for (const auto& diagnostic : invalid_numeric_loaded_specs.diagnostics) {
        saw_invalid_memory = saw_invalid_memory || diagnostic.reason.find("memory_limit_bytes") != std::string::npos;
        saw_invalid_bool = saw_invalid_bool || diagnostic.reason.find("idempotent") != std::string::npos;
        saw_negative_timeout = saw_negative_timeout || diagnostic.reason.find("timeout_ms must be >= 1") != std::string::npos;
        saw_unused_required_arg =
            saw_unused_required_arg || diagnostic.reason.find("required_arg is not referenced") != std::string::npos;
        saw_invalid_schema = saw_invalid_schema || diagnostic.reason.find("input_schema_json") != std::string::npos;
        saw_invalid_risk = saw_invalid_risk || diagnostic.reason.find("unsupported risk_level") != std::string::npos;
        saw_invalid_permission = saw_invalid_permission || diagnostic.reason.find("unknown permissions") != std::string::npos;
        saw_missing_spawn_permission =
            saw_missing_spawn_permission || diagnostic.reason.find("permissions must include process.spawn") != std::string::npos;
        saw_invalid_health_arg =
            saw_invalid_health_arg || diagnostic.reason.find("health_args_template references runtime argument") != std::string::npos;
        saw_invalid_sandbox = saw_invalid_sandbox || diagnostic.reason.find("unsupported sandbox_mode") != std::string::npos;
        saw_invalid_lifecycle = saw_invalid_lifecycle || diagnostic.reason.find("unsupported lifecycle_mode") != std::string::npos;
        saw_persistent_stdio =
            saw_persistent_stdio || diagnostic.reason.find("persistent plugin lifecycle requires json-rpc-v0") != std::string::npos;
    }
    Expect(saw_invalid_memory, "plugin numeric diagnostic should identify the invalid memory_limit_bytes field");
    Expect(saw_invalid_bool, "plugin boolean diagnostic should identify the invalid idempotent field");
    Expect(saw_negative_timeout, "plugin numeric diagnostic should reject negative timeout_ms values");
    Expect(saw_unused_required_arg, "plugin loader should reject unused required_args");
    Expect(saw_invalid_schema, "plugin loader should reject invalid input_schema_json values");
    Expect(saw_invalid_risk, "plugin loader should reject unsupported risk levels");
    Expect(saw_invalid_permission, "plugin loader should reject unknown permissions");
    Expect(saw_missing_spawn_permission, "plugin loader should require process.spawn permission");
    Expect(saw_invalid_health_arg, "plugin loader should reject health probe templates that require runtime arguments");
    Expect(saw_invalid_sandbox, "plugin loader should reject unsupported sandbox modes");
    Expect(saw_invalid_lifecycle, "plugin loader should reject unsupported lifecycle modes");
    Expect(saw_persistent_stdio, "plugin loader should require json-rpc-v0 for persistent lifecycle mode");

    agentos::PluginSpec invalid_protocol_spec{
        .manifest_version = "plugin.v1",
        .name = "bad_protocol_plugin",
        .description = "Invalid protocol fixture.",
        .binary = binary,
        .args_template = std::vector<std::string>{},
        .required_args = std::vector<std::string>{},
        .protocol = "json-rpc-v9",
        .memory_limit_bytes = 268435456,
        .max_processes = 16,
        .cpu_time_limit_seconds = 10,
        .file_descriptor_limit = 512,
    };
    agentos::PluginSkillInvoker invalid_protocol_invoker(invalid_protocol_spec, plugin_host);
    Expect(!invalid_protocol_invoker.healthy(), "plugin invoker should be unhealthy for unsupported protocol");
    const auto invalid_protocol_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = invalid_protocol_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!invalid_protocol_run.success, "plugin host should reject unsupported protocol at run time");
    Expect(invalid_protocol_run.error_code == "InvalidPluginSpec", "unsupported plugin protocol should return InvalidPluginSpec");

    agentos::PluginSpec invalid_version_spec = invalid_protocol_spec;
    invalid_version_spec.name = "bad_version_plugin";
    invalid_version_spec.protocol = "stdio-json-v0";
    invalid_version_spec.manifest_version = "plugin.v9";
    agentos::PluginSkillInvoker invalid_version_invoker(invalid_version_spec, plugin_host);
    Expect(!invalid_version_invoker.healthy(), "plugin invoker should be unhealthy for unsupported manifest version");
    const auto invalid_version_run = plugin_host.run(agentos::PluginRunRequest{
        .spec = invalid_version_spec,
        .workspace_path = isolated_workspace,
    });
    Expect(!invalid_version_run.success, "plugin host should reject unsupported manifest version at run time");
    Expect(invalid_version_run.error_code == "InvalidPluginSpec", "unsupported plugin version should return InvalidPluginSpec");

    agentos::PluginSpec missing_binary_spec{
        .manifest_version = "plugin.v1",
        .name = "missing_binary_plugin",
        .description = "Missing binary fixture.",
        .binary = "agentos_missing_plugin_binary_for_test",
        .protocol = "stdio-json-v0",
        .permissions = {"process.spawn"},
    };
    const auto missing_binary_health = agentos::CheckPluginHealth(missing_binary_spec);
    Expect(missing_binary_health.supported, "plugin health should support a valid spec even when its binary is missing");
    Expect(!missing_binary_health.command_available, "plugin health should report unavailable plugin binaries");
    Expect(!missing_binary_health.healthy, "plugin health should mark missing plugin binaries as unhealthy");
    Expect(missing_binary_health.reason.find("plugin binary not found") != std::string::npos,
        "plugin health should explain missing plugin binaries");
}

void TestPluginPoolPolicyAndAdmin(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "plugin_pool_admin_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

#ifdef _WIN32
    const auto binary = "powershell";
    const auto script_path = isolated_workspace / "pool_admin_session.ps1";
    {
        std::ofstream script(script_path, std::ios::binary);
        script
            << "$counter = 0\n"
            << "while (($line = [Console]::In.ReadLine()) -ne $null) {\n"
            << "  $counter += 1\n"
            << "  Write-Output \"{\"\"jsonrpc\"\":\"\"2.0\"\",\"\"id\"\":$counter,\"\"result\"\":{\"\"message\"\":\"\"pool-$counter\"\"}}\"\n"
            << "  [Console]::Out.Flush()\n"
            << "}\n";
    }
    const std::vector<std::string> persistent_args = {
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script_path.string()};
#else
    const auto binary = "sh";
    const auto script_path = isolated_workspace / "pool_admin_session.sh";
    {
        std::ofstream script(script_path, std::ios::binary);
        script
            << "#!/usr/bin/env sh\n"
            << "counter=0\n"
            << "while IFS= read -r line; do\n"
            << "  counter=$((counter + 1))\n"
            << "  printf '{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"message\":\"pool-%s\"}}\\n' \"$counter\" \"$counter\"\n"
            << "done\n";
    }
    std::filesystem::permissions(
        script_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
    const std::vector<std::string> persistent_args = {script_path.string()};
#endif

    agentos::PluginSpec pool_spec;
    pool_spec.manifest_version = "plugin.v1";
    pool_spec.name = "pool_admin_session";
    pool_spec.description = "Pool/admin fixture";
    pool_spec.binary = binary;
    pool_spec.args_template = persistent_args;
    pool_spec.protocol = "json-rpc-v0";
    pool_spec.permissions = {"process.spawn"};
    pool_spec.lifecycle_mode = "persistent";
    pool_spec.timeout_ms = 1000;
    pool_spec.startup_timeout_ms = 1500;
    pool_spec.idle_timeout_ms = 60000;
    pool_spec.pool_size = 1;

    agentos::CliHost cli_host;
    agentos::PluginHost host(cli_host, agentos::PluginHostOptions{.max_persistent_sessions = 16});

    // First call: workspace A, should start a fresh persistent session.
    const auto workspace_a = isolated_workspace / "ws_a";
    std::filesystem::create_directories(workspace_a);
    const auto first_run = host.run(agentos::PluginRunRequest{
        .spec = pool_spec,
        .workspace_path = workspace_a,
    });
    Expect(first_run.success, "pool fixture should start a session in workspace A");
    Expect(host.active_session_count() == 1, "pool host should hold one active session after first run");
    const auto sessions_after_first = host.list_sessions();
    Expect(sessions_after_first.size() == 1, "list_sessions should report one entry after first run");
    if (!sessions_after_first.empty()) {
        const auto& info = sessions_after_first.front();
        Expect(info.plugin_name == "pool_admin_session",
            "session info should expose the plugin name");
        Expect(info.pid > 0, "session info should expose a non-zero pid for an alive process");
        Expect(info.request_count >= 1,
            "session info request_count should reflect the executed request");
        Expect(info.alive, "session info alive flag should be true for a running process");
    }

    // Second call: workspace B with pool_size=1 → must evict the workspace-A session.
    const auto workspace_b = isolated_workspace / "ws_b";
    std::filesystem::create_directories(workspace_b);
    const auto second_run = host.run(agentos::PluginRunRequest{
        .spec = pool_spec,
        .workspace_path = workspace_b,
    });
    Expect(second_run.success, "pool fixture should start a second session in workspace B");
    Expect(host.active_session_count() == 1,
        "pool_size=1 should evict the older session for the same plugin");
    const auto sessions_after_second = host.list_sessions();
    bool saw_workspace_b = false;
    for (const auto& info : sessions_after_second) {
        if (info.workspace_path.find("ws_b") != std::string::npos) {
            saw_workspace_b = true;
        }
    }
    Expect(saw_workspace_b, "remaining session should be the workspace B session after pool_size=1 eviction");

    // pool_size=2 → both workspace A and B sessions can coexist.
    pool_spec.pool_size = 2;
    const auto wide_first = host.run(agentos::PluginRunRequest{
        .spec = pool_spec,
        .workspace_path = workspace_a,
    });
    Expect(wide_first.success, "pool_size=2 fixture should start a workspace-A session");
    Expect(host.active_session_count() == 2,
        "pool_size=2 should allow two concurrent persistent sessions for one plugin");

    // session-close: forcibly close all sessions for the plugin.
    const auto closed = host.close_sessions_for_plugin("pool_admin_session");
    Expect(closed == 2, "close_sessions_for_plugin should close every matching session");
    Expect(host.active_session_count() == 0,
        "no sessions should remain after close_sessions_for_plugin");

    // session-restart: start a session, then force-restart and verify the request counter resets.
    pool_spec.pool_size = 1;
    const auto restart_seed = host.run(agentos::PluginRunRequest{
        .spec = pool_spec,
        .workspace_path = workspace_a,
    });
    Expect(restart_seed.success, "restart seed run should start a fresh session");
    const auto sessions_before_restart = host.list_sessions();
    int seed_request_count = 0;
    if (!sessions_before_restart.empty()) {
        seed_request_count = sessions_before_restart.front().request_count;
    }
    Expect(seed_request_count >= 1,
        "session info should report at least one request before restart");
    const auto restarted_count = host.restart_sessions_for_plugin("pool_admin_session");
    Expect(restarted_count >= 1,
        "restart_sessions_for_plugin should restart the live session");
    const auto sessions_after_restart = host.list_sessions();
    Expect(sessions_after_restart.size() == 1,
        "host should still hold exactly one session after restart");
    if (!sessions_after_restart.empty()) {
        Expect(sessions_after_restart.front().request_count == 0,
            "restarted session should have request_count reset to zero");
    }
    const auto post_restart_run = host.run(agentos::PluginRunRequest{
        .spec = pool_spec,
        .workspace_path = workspace_a,
    });
    Expect(post_restart_run.success,
        "session should remain usable after restart_sessions_for_plugin");
    Expect(post_restart_run.stdout_text.find("pool-1") != std::string::npos,
        "post-restart session should resume the response counter from one");
    Expect(post_restart_run.lifecycle_event == "reused",
        "post-restart session should report reused lifecycle event for a live restarted session");

    // close_sessions_for_plugin with an unknown plugin reports zero.
    Expect(host.close_sessions_for_plugin("does_not_exist") == 0,
        "close_sessions_for_plugin should return zero for unknown plugins");

    Expect(host.close_all_sessions() >= 1,
        "close_all_sessions should drain the remaining session");

    // Manifest pool_size parsing: TSV.
    const auto pool_workspace = isolated_workspace / "manifest_pool";
    std::filesystem::create_directories(pool_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(pool_workspace / "runtime" / "plugin_specs" / "pool_size_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "pool_size_plugin" << '\t'
            << "Pool size manifest fixture." << '\t'
            << binary << '\t'
#ifdef _WIN32
            << "/d,/s,/c,echo {}" << '\t'
#else
            << "-c,printf '{}\\n'" << '\t'
#endif
            << "" << '\t'
            << "json-rpc-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "4096" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "workspace" << '\t'
            << "persistent" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "4"
            << '\n';
    }
    const auto loaded = agentos::LoadPluginSpecsWithDiagnostics(pool_workspace / "runtime" / "plugin_specs");
    Expect(loaded.diagnostics.empty(),
        "pool_size manifest fixture should load without diagnostics");
    Expect(loaded.specs.size() == 1, "pool_size manifest fixture should yield one spec");
    if (!loaded.specs.empty()) {
        Expect(loaded.specs.front().pool_size == 4,
            "TSV plugin manifest should parse pool_size=4");
    }

    // Manifest pool_size parsing: JSON.
    const auto json_pool_workspace = isolated_workspace / "manifest_pool_json";
    std::filesystem::create_directories(json_pool_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(
            json_pool_workspace / "runtime" / "plugin_specs" / "pool_size_json.json",
            std::ios::binary);
        spec_file
            << "{\n"
            << R"(  "manifest_version": "plugin.v1",)" << '\n'
            << R"(  "name": "pool_size_json",)" << '\n'
            << R"(  "description": "JSON pool_size fixture.",)" << '\n'
            << R"(  "binary": ")" << binary << R"(",)" << '\n'
#ifdef _WIN32
            << R"(  "args_template": ["/d", "/s", "/c", "echo {}"],)" << '\n'
#else
            << R"(  "args_template": ["-c", "printf '{}\\n'"],)" << '\n'
#endif
            << R"(  "protocol": "json-rpc-v0",)" << '\n'
            << R"(  "permissions": ["process.spawn"],)" << '\n'
            << R"(  "lifecycle_mode": "persistent",)" << '\n'
            << R"(  "pool_size": 3)" << '\n'
            << "}\n";
    }
    const auto loaded_json = agentos::LoadPluginSpecsWithDiagnostics(json_pool_workspace / "runtime" / "plugin_specs");
    Expect(loaded_json.diagnostics.empty(),
        "JSON pool_size manifest should load without diagnostics");
    if (!loaded_json.specs.empty()) {
        Expect(loaded_json.specs.front().pool_size == 3,
            "JSON plugin manifest should parse pool_size=3");
    }
}

void TestJqTransformCliSkillWithFixture(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "jq_transform_fixture_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "bin");
    std::filesystem::create_directories(isolated_workspace / "data");

    WriteJqCliFixture(isolated_workspace / "bin");
    {
        std::ofstream input(isolated_workspace / "data" / "input.json", std::ios::binary);
        input << R"({"name":"agentos"})";
    }

    const auto fixture_path = PrependPathForTest(isolated_workspace / "bin");
    ScopedEnvOverride path_override("PATH", fixture_path);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);
    agentos::CliHost cli_host;
    runtime.skill_registry.register_skill(std::make_shared<agentos::CliSkillInvoker>(
        agentos::MakeJqTransformSpec(), cli_host));

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "jq-transform-fixture",
        .task_type = "jq_transform",
        .objective = "transform json through jq fixture",
        .workspace_path = isolated_workspace,
        .inputs = {
            {"filter", ".name"},
            {"path", "data/input.json"},
        },
    });

    Expect(result.success, "jq_transform should execute through CliSkillInvoker");
    Expect(result.route_target == "jq_transform", "jq_transform should route by skill name");
    Expect(result.output_json.find(R"({\"fixture\":\"jq\",\"filter\":\".name\"})") != std::string::npos,
        "jq_transform output should include jq fixture JSON");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestCliHostEnvironmentAllowlist(workspace);
    TestCliHostRedactsSensitiveArguments(workspace);
    TestCliHostProcessLimit(workspace);
    TestCliHostTimeoutKillsProcessTree(workspace);
    TestExternalCliSpecLoader(workspace);
    TestPluginSpecLoaderAndInvoker(workspace);
    TestPluginPoolPolicyAndAdmin(workspace);
    TestJqTransformCliSkillWithFixture(workspace);

    if (failures != 0) {
        std::cerr << failures << " cli/plugin test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_cli_plugin_tests passed\n";
    return 0;
}
