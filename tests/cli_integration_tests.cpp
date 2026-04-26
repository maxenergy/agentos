#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace(const std::string& name) {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_cli_tests" / name;
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

std::string QuoteShellArg(const std::string& value) {
    if (value.find_first_of(" \t\n\"&<>|^") == std::string::npos) {
        return value;
    }

    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (const char ch : value) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back('"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, '\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult RunAgentos(const std::filesystem::path& workspace, const std::vector<std::string>& args) {
    std::ostringstream command;
#ifdef _WIN32
    command << "cd /d " << QuoteShellArg(workspace.string()) << " && ";
#else
    command << "cd " << QuoteShellArg(workspace.string()) << " && ";
#endif
    command << QuoteShellArg(AGENTOS_CLI_TEST_EXE);
    for (const auto& arg : args) {
        command << ' ' << QuoteShellArg(arg);
    }
    command << " 2>&1";

#ifdef _WIN32
    FILE* pipe = _popen(command.str().c_str(), "r");
#else
    FILE* pipe = popen(command.str().c_str(), "r");
#endif
    if (!pipe) {
        return {.exit_code = -1, .output = "failed to open process"};
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    return {.exit_code = status, .output = std::move(output)};
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string ExtractTokenValue(const std::string& text, const std::string& key) {
    const auto start = text.find(key);
    if (start == std::string::npos) {
        return "";
    }
    const auto value_start = start + key.size();
    const auto value_end = text.find_first_of(" \r\n", value_start);
    return text.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
}

std::optional<std::string> ReadEnvForTest(const std::string& name) {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, name.c_str()) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }
    std::string value(raw_value, value_size > 0 ? value_size - 1 : 0);
    std::free(raw_value);
    return value;
#else
    const char* value = std::getenv(name.c_str());
    if (!value) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

void SetEnvForTest(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

char PathListSeparatorForTest() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

void WriteCurlTokenFixture(const std::filesystem::path& bin_dir) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "curl.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "echo {\"access_token\":\"cli-access\",\"refresh_token\":\"cli-refresh\",\"token_type\":\"Bearer\",\"expires_in\":900}\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "curl";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "printf '%s\\n' '{\"access_token\":\"cli-access\",\"refresh_token\":\"cli-refresh\",\"token_type\":\"Bearer\",\"expires_in\":900}'\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
}

void TestAgentsCommand() {
    const auto workspace = FreshWorkspace("agents");
    const auto result = RunAgentos(workspace, {"agents"});
    Expect(result.exit_code == 0, "agents command should succeed");
    Expect(result.output.find("local_planner") != std::string::npos, "agents command should list local_planner");
    Expect(result.output.find("codex_cli") != std::string::npos, "agents command should list codex_cli");
}

void TestPluginsCommand() {
    const auto workspace = FreshWorkspace("plugins");
    std::filesystem::create_directories(workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
    const auto health_args_template = "/d,/s,/c,exit 0";
    const auto failing_health_args_template = "/d,/s,/c,exit 7";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
    const auto health_args_template = "-c,exit 0";
    const auto failing_health_args_template = "-c,exit 7";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "cli_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "cli_plugin" << '\t'
            << "CLI plugin health fixture." << '\t'
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
            << health_args_template << '\t'
            << "1000"
            << '\n';
    }
    {
#ifdef _WIN32
        const auto json_args_template = R"(  "args_template": ["/d", "/s", "/c", "echo {{message}}"],)";
        const auto json_health_args_template = R"(  "health_args_template": ["/d", "/s", "/c", "exit 0"],)";
#else
        const auto json_args_template = R"(  "args_template": ["-c", "printf '%s\\n' \"{{message}}\""],)";
        const auto json_health_args_template = R"(  "health_args_template": ["-c", "exit 0"],)";
#endif
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "json_cli_plugin.json", std::ios::binary);
        spec_file
            << "{\n"
            << R"(  "manifest_version": "plugin.v1",)" << '\n'
            << R"(  "name": "json_cli_plugin",)" << '\n'
            << R"(  "description": "JSON CLI plugin health fixture.",)" << '\n'
            << R"(  "binary": ")" << binary << R"(",)" << '\n'
            << json_args_template << '\n'
            << R"(  "required_args": ["message"],)" << '\n'
            << R"(  "protocol": "stdio-json-v0",)" << '\n'
            << R"(  "input_schema_json": {"type": "object"},)" << '\n'
            << R"(  "output_schema_json": {"type": "object"},)" << '\n'
            << R"(  "risk_level": "low",)" << '\n'
            << R"(  "permissions": ["process.spawn"],)" << '\n'
            << R"(  "timeout_ms": 3000,)" << '\n'
            << R"(  "health_timeout_ms": 1000,)" << '\n'
            << json_health_args_template << '\n'
            << "}\n";
    }

    const auto result = RunAgentos(workspace, {"plugins"});
    Expect(result.exit_code == 0, "plugins command should succeed");
    Expect(result.output.find("cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins command should list loaded plugin specs");
    Expect(result.output.find("json_cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins command should list loaded JSON plugin specs");
    Expect(result.output.find("source=") != std::string::npos && result.output.find("cli_plugin.tsv") != std::string::npos,
        "plugins command should include loaded plugin source file paths");
    Expect(result.output.find("json_cli_plugin.json") != std::string::npos && result.output.find("line=1") != std::string::npos,
        "plugins command should include loaded JSON plugin source line");
    Expect(result.output.find("lifecycle_mode=oneshot") != std::string::npos,
        "plugins command should include plugin lifecycle mode");
    Expect(result.output.find("startup_timeout_ms=3000") != std::string::npos,
        "plugins command should include plugin startup timeout");
    Expect(result.output.find("idle_timeout_ms=30000") != std::string::npos,
        "plugins command should include plugin idle timeout");
    Expect(result.output.find("healthy=true") != std::string::npos,
        "plugins command should include plugin health");

    const auto validate = RunAgentos(workspace, {"plugins", "validate"});
    Expect(validate.exit_code == 0, "plugins validate should succeed when every plugin spec is valid");
    Expect(validate.output.find("cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins validate should list valid plugin specs");
    Expect(validate.output.find("json_cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins validate should list valid JSON plugin specs");
    Expect(validate.output.find("source=") != std::string::npos && validate.output.find("cli_plugin.tsv") != std::string::npos,
        "plugins validate should include loaded plugin source file paths");
    Expect(validate.output.find("valid=true") != std::string::npos,
        "plugins validate should report valid loaded plugin specs");

    const auto health = RunAgentos(workspace, {"plugins", "health"});
    Expect(health.exit_code == 0, "plugins health should succeed when every loaded plugin is healthy");
    Expect(health.output.find("cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should print the same plugin health listing");
    Expect(health.output.find("json_cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should print JSON plugin health listing");
    Expect(health.output.find("source=") != std::string::npos && health.output.find("json_cli_plugin.json") != std::string::npos,
        "plugins health should include loaded plugin source file paths");

    const auto inspect = RunAgentos(workspace, {"plugins", "inspect", "name=cli_plugin"});
    Expect(inspect.exit_code == 0, "plugins inspect should succeed for a loaded plugin");
    Expect(inspect.output.find("name=cli_plugin") != std::string::npos,
        "plugins inspect should print the plugin name");
    Expect(inspect.output.find("protocol=stdio-json-v0") != std::string::npos,
        "plugins inspect should print the plugin protocol");
    Expect(inspect.output.find("required_args=message") != std::string::npos,
        "plugins inspect should print required args");
    Expect(inspect.output.find("permissions=process.spawn") != std::string::npos,
        "plugins inspect should print permissions");
    Expect(inspect.output.find("lifecycle_mode=oneshot") != std::string::npos,
        "plugins inspect should print lifecycle mode");
    Expect(inspect.output.find("valid=true") != std::string::npos,
        "plugins inspect should report loaded plugin validity");

    const auto inspect_health = RunAgentos(workspace, {"plugins", "inspect", "name=cli_plugin", "health=true"});
    Expect(inspect_health.exit_code == 0, "plugins inspect health should succeed for a healthy plugin");
    Expect(inspect_health.output.find("healthy=true") != std::string::npos,
        "plugins inspect health should report healthy plugins");
    Expect(inspect_health.output.find("command_available=true") != std::string::npos,
        "plugins inspect health should report command availability");

    const auto missing_inspect = RunAgentos(workspace, {"plugins", "inspect", "name=missing_plugin"});
    Expect(missing_inspect.exit_code != 0, "plugins inspect should fail for a missing plugin");
    Expect(missing_inspect.output.find("plugin_not_found name=missing_plugin") != std::string::npos,
        "plugins inspect should report the missing plugin name");

    const auto lifecycle = RunAgentos(workspace, {"plugins", "lifecycle"});
    Expect(lifecycle.exit_code == 0, "plugins lifecycle should succeed for valid plugin specs");
    Expect(lifecycle.output.find("plugin_lifecycle name=cli_plugin lifecycle_mode=oneshot") != std::string::npos,
        "plugins lifecycle should list per-plugin lifecycle settings");
    Expect(lifecycle.output.find("plugin_lifecycle_summary total=2 oneshot=2 persistent=0") != std::string::npos,
        "plugins lifecycle should summarize lifecycle counts");

    const auto lifecycle_workspace = FreshWorkspace("plugins_lifecycle");
    std::filesystem::create_directories(lifecycle_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream options_file(lifecycle_workspace / "runtime" / "plugin_host.tsv", std::ios::binary);
        options_file << "max_persistent_sessions\t3\n";
    }
    {
        std::ofstream spec_file(lifecycle_workspace / "runtime" / "plugin_specs" / "persistent_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "persistent_plugin" << '\t'
            << "Persistent plugin lifecycle fixture." << '\t'
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
            << "3000" << '\t'
            << "workspace" << '\t'
            << "persistent" << '\t'
            << "1500" << '\t'
            << "60000"
            << '\n';
    }
    const auto persistent_lifecycle = RunAgentos(lifecycle_workspace, {"plugins", "lifecycle"});
    Expect(persistent_lifecycle.exit_code == 0, "plugins lifecycle should not require starting persistent plugin processes");
    Expect(persistent_lifecycle.output.find("plugin_lifecycle name=persistent_plugin lifecycle_mode=persistent protocol=json-rpc-v0") != std::string::npos,
        "plugins lifecycle should report persistent JSON-RPC plugins");
    Expect(persistent_lifecycle.output.find("persistent=1") != std::string::npos,
        "plugins lifecycle summary should count persistent plugins");
    Expect(persistent_lifecycle.output.find("max_persistent_sessions=3") != std::string::npos,
        "plugins lifecycle summary should report workspace-configured persistent session cap");

    const auto invalid_lifecycle_workspace = FreshWorkspace("plugins_lifecycle_invalid");
    std::filesystem::create_directories(invalid_lifecycle_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream options_file(invalid_lifecycle_workspace / "runtime" / "plugin_host.tsv", std::ios::binary);
        options_file << "max_persistent_sessions\tinvalid\n";
    }
    const auto invalid_lifecycle = RunAgentos(invalid_lifecycle_workspace, {"plugins", "lifecycle"});
    Expect(invalid_lifecycle.exit_code != 0, "plugins lifecycle should fail when plugin host config is invalid");
    Expect(invalid_lifecycle.output.find("plugin_host_config_diagnostic line=1") != std::string::npos,
        "plugins lifecycle should print plugin host config diagnostics");
    Expect(invalid_lifecycle.output.find("config_diagnostics=1") != std::string::npos,
        "plugins lifecycle summary should count plugin host config diagnostics");

    const auto unhealthy_workspace = FreshWorkspace("plugins_unhealthy");
    std::filesystem::create_directories(unhealthy_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(unhealthy_workspace / "runtime" / "plugin_specs" / "missing_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "missing_plugin" << '\t'
            << "Missing plugin binary fixture." << '\t'
            << "agentos_missing_plugin_binary_for_cli_test" << '\t'
            << "{{message}}" << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto unhealthy = RunAgentos(unhealthy_workspace, {"plugins", "health"});
    Expect(unhealthy.exit_code != 0, "plugins health should fail when any loaded plugin is unhealthy");
    Expect(unhealthy.output.find("missing_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should list unhealthy plugins");
    Expect(unhealthy.output.find("healthy=false") != std::string::npos,
        "plugins health should report unhealthy plugin state");

    const auto unhealthy_validate = RunAgentos(unhealthy_workspace, {"plugins", "validate"});
    Expect(unhealthy_validate.exit_code == 0,
        "plugins validate should not fail solely because a valid plugin binary is unavailable");
    Expect(unhealthy_validate.output.find("missing_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins validate should still list valid specs whose binary is unavailable");

    const auto unhealthy_inspect = RunAgentos(unhealthy_workspace, {"plugins", "inspect", "name=missing_plugin", "health=true"});
    Expect(unhealthy_inspect.exit_code != 0, "plugins inspect health should fail for an unhealthy plugin");
    Expect(unhealthy_inspect.output.find("name=missing_plugin") != std::string::npos,
        "plugins inspect health should print the unhealthy plugin name");
    Expect(unhealthy_inspect.output.find("healthy=false") != std::string::npos,
        "plugins inspect health should report unhealthy plugins");
    Expect(unhealthy_inspect.output.find("command_available=false") != std::string::npos,
        "plugins inspect health should report unavailable commands");

    const auto failed_probe_workspace = FreshWorkspace("plugins_failed_probe");
    std::filesystem::create_directories(failed_probe_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(failed_probe_workspace / "runtime" / "plugin_specs" / "failed_probe_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "failed_probe_plugin" << '\t'
            << "Plugin health probe failure fixture." << '\t'
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
            << failing_health_args_template << '\t'
            << "1000"
            << '\n';
    }
    const auto failed_probe = RunAgentos(failed_probe_workspace, {"plugins", "health"});
    Expect(failed_probe.exit_code != 0, "plugins health should fail when a declared health probe fails");
    Expect(failed_probe.output.find("failed_probe_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should list plugins with failed health probes");
    Expect(failed_probe.output.find("health probe failed") != std::string::npos,
        "plugins health should explain failed health probes");

    const auto invalid_workspace = FreshWorkspace("plugins_invalid");
    std::filesystem::create_directories(invalid_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "plugin_specs" / "invalid_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_plugin" << '\t'
            << "Invalid plugin protocol fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "json-rpc-v9" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto invalid = RunAgentos(invalid_workspace, {"plugins", "health"});
    Expect(invalid.exit_code != 0, "plugins health should fail when plugin specs are skipped as invalid");
    Expect(invalid.output.find("skipped_plugin") != std::string::npos,
        "plugins health should print diagnostics for skipped plugin specs");
    Expect(invalid.output.find("unsupported plugin protocol") != std::string::npos,
        "plugins health should explain skipped plugin specs");

    const auto invalid_validate = RunAgentos(invalid_workspace, {"plugins", "validate"});
    Expect(invalid_validate.exit_code != 0, "plugins validate should fail when plugin specs are invalid");
    Expect(invalid_validate.output.find("skipped_plugin") != std::string::npos,
        "plugins validate should print diagnostics for skipped plugin specs");

    const auto invalid_audit = ReadTextFile(invalid_workspace / "runtime" / "audit.log");
    Expect(invalid_audit.find(R"("event":"config_diagnostic")") != std::string::npos,
        "startup should audit skipped plugin spec diagnostics");
    Expect(invalid_audit.find(R"("source":"plugin_spec")") != std::string::npos,
        "startup plugin diagnostic audit should include the source");
    Expect(invalid_audit.find("unsupported plugin protocol") != std::string::npos,
        "startup plugin diagnostic audit should include the skip reason");
}

void TestCliSpecsCommand() {
    const auto workspace = FreshWorkspace("cli_specs");
    std::filesystem::create_directories(workspace / "runtime" / "cli_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "cli_specs" / "valid_cli.tsv", std::ios::binary);
        spec_file
            << "valid_cli" << '\t'
            << "Valid external CLI fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto valid = RunAgentos(workspace, {"cli-specs", "validate"});
    Expect(valid.exit_code == 0, "cli-specs validate should succeed when every external CLI spec is valid");
    Expect(valid.output.find("valid_cli parse_mode=text") != std::string::npos,
        "cli-specs validate should list valid external CLI specs");
    Expect(valid.output.find("valid=true") != std::string::npos,
        "cli-specs validate should report valid specs");

    const auto invalid_workspace = FreshWorkspace("cli_specs_invalid");
    std::filesystem::create_directories(invalid_workspace / "runtime" / "cli_specs");
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_cli.tsv", std::ios::binary);
        spec_file << "too" << '\t' << "few" << '\n';
    }

    const auto invalid = RunAgentos(invalid_workspace, {"cli-specs", "validate"});
    Expect(invalid.exit_code != 0, "cli-specs validate should fail when an external CLI spec is invalid");
    Expect(invalid.output.find("skipped_cli_spec") != std::string::npos,
        "cli-specs validate should print diagnostics for skipped external CLI specs");
    Expect(invalid.output.find("CLI spec requires at least 9 fields") != std::string::npos,
        "cli-specs validate should explain skipped external CLI specs");

    const auto invalid_audit = ReadTextFile(invalid_workspace / "runtime" / "audit.log");
    Expect(invalid_audit.find(R"("event":"config_diagnostic")") != std::string::npos,
        "startup should audit skipped external CLI spec diagnostics");
    Expect(invalid_audit.find(R"("source":"cli_spec")") != std::string::npos,
        "startup CLI spec diagnostic audit should include the source");
    Expect(invalid_audit.find("CLI spec requires at least 9 fields") != std::string::npos,
        "startup CLI spec diagnostic audit should include the skip reason");
}

void TestSpecNameConflictsAreAudited() {
    const auto workspace = FreshWorkspace("spec_name_conflicts");
    std::filesystem::create_directories(workspace / "runtime" / "cli_specs");
    std::filesystem::create_directories(workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "cli_specs" / "file_read_conflict.tsv", std::ios::binary);
        spec_file
            << "file_read" << '\t'
            << "Conflicts with builtin file_read." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "file_write_conflict.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "file_write" << '\t'
            << "Conflicts with builtin file_write." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto result = RunAgentos(workspace, {"agents"});
    Expect(result.exit_code == 0, "agents command should still succeed when external specs conflict with builtin skills");

    const auto cli_validate = RunAgentos(workspace, {"cli-specs", "validate"});
    Expect(cli_validate.exit_code != 0,
        "cli-specs validate should fail when external CLI specs conflict with registered skills");
    Expect(cli_validate.output.find("external CLI spec name conflicts with already registered skill: file_read") != std::string::npos,
        "cli-specs validate should explain conflicts with registered skills");
    Expect(cli_validate.output.find("conflicting_cli_spec") != std::string::npos,
        "cli-specs validate should print source diagnostics for conflicts with registered skills");
    Expect(cli_validate.output.find("file_read_conflict.tsv") != std::string::npos,
        "cli-specs validate conflict diagnostics should include source file names");

    const auto plugin_validate = RunAgentos(workspace, {"plugins", "validate"});
    Expect(plugin_validate.exit_code != 0,
        "plugins validate should fail when plugin specs conflict with registered skills");
    Expect(plugin_validate.output.find("plugin spec name conflicts with already registered skill: file_write") != std::string::npos,
        "plugins validate should explain conflicts with registered skills");
    Expect(plugin_validate.output.find("conflicting_plugin") != std::string::npos,
        "plugins validate should print source diagnostics for conflicts with registered skills");
    Expect(plugin_validate.output.find("file_write_conflict.tsv") != std::string::npos,
        "plugins validate conflict diagnostics should include source file names");

    const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
    Expect(audit.find("external CLI spec name conflicts with already registered skill: file_read") != std::string::npos,
        "startup should audit external CLI spec conflicts with builtin skills");
    Expect(audit.find("plugin spec name conflicts with already registered skill: file_write") != std::string::npos,
        "startup should audit plugin spec conflicts with builtin skills");
}

void TestPluginNameConflictsWithExternalCliSpec() {
    const auto workspace = FreshWorkspace("plugin_cli_name_conflicts");
    std::filesystem::create_directories(workspace / "runtime" / "cli_specs");
    std::filesystem::create_directories(workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "cli_specs" / "shared_tool.tsv", std::ios::binary);
        spec_file
            << "shared_tool" << '\t'
            << "Valid external CLI fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "shared_tool.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "shared_tool" << '\t'
            << "Conflicts with external CLI shared_tool." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto cli_validate = RunAgentos(workspace, {"cli-specs", "validate"});
    Expect(cli_validate.exit_code == 0,
        "cli-specs validate should succeed when the external CLI spec itself is valid");

    const auto plugin_validate = RunAgentos(workspace, {"plugins", "validate"});
    Expect(plugin_validate.exit_code != 0,
        "plugins validate should fail when plugin specs conflict with valid external CLI specs");
    Expect(plugin_validate.output.find("plugin spec name conflicts with already registered skill: shared_tool") != std::string::npos,
        "plugins validate should explain conflicts with external CLI specs");
    Expect(plugin_validate.output.find("conflicting_plugin") != std::string::npos,
        "plugins validate should print source diagnostics for plugin/external CLI conflicts");

    const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
    Expect(audit.find("plugin spec name conflicts with already registered skill: shared_tool") != std::string::npos,
        "startup should audit plugin spec conflicts with external CLI specs");
}

void TestMemoryAndStorageCommands() {
    const auto workspace = FreshWorkspace("memory_storage");

    const auto memory = RunAgentos(workspace, {"memory", "summary"});
    Expect(memory.exit_code == 0, "memory summary command should succeed");
    Expect(memory.output.find("tasks=0") != std::string::npos, "memory summary should start with zero tasks in a fresh workspace");

    const auto first_write = RunAgentos(workspace, {
        "run", "write_file", "path=memory_storage/workflow_a.txt", "content=a", "idempotency_key=workflow-cli-a"});
    Expect(first_write.exit_code == 0, "first workflow candidate write should succeed");
    const auto second_write = RunAgentos(workspace, {
        "run", "write_file", "path=memory_storage/workflow_b.txt", "content=b", "idempotency_key=workflow-cli-b"});
    Expect(second_write.exit_code == 0, "second workflow candidate write should succeed");
    const auto promote = RunAgentos(workspace, {
        "memory", "promote-workflow", "write_file_workflow", "input_regex=branch=release/.*"});
    Expect(promote.exit_code == 0, "memory promote-workflow should accept input_regex conditions");
    Expect(promote.output.find("input_regex=branch=release/.*") != std::string::npos,
        "promote-workflow output should include input_regex conditions");

    const auto show_workflow = RunAgentos(workspace, {"memory", "show-workflow", "write_file_workflow"});
    Expect(show_workflow.exit_code == 0, "memory show-workflow should succeed for existing workflows");
    Expect(show_workflow.output.find("write_file_workflow") != std::string::npos,
        "memory show-workflow should print the selected workflow name");
    Expect(show_workflow.output.find("steps=file_write") != std::string::npos,
        "memory show-workflow should print ordered workflow steps");
    Expect(show_workflow.output.find("input_regex=branch=release/.*") != std::string::npos,
        "memory show-workflow should print workflow applicability conditions");

    const auto show_missing_workflow = RunAgentos(workspace, {"memory", "show-workflow", "missing_workflow"});
    Expect(show_missing_workflow.exit_code != 0, "memory show-workflow should fail for missing workflows");
    Expect(show_missing_workflow.output.find("workflow not found: missing_workflow") != std::string::npos,
        "memory show-workflow should explain missing workflow failures");

    const auto stored_enabled_workflows = RunAgentos(workspace, {
        "memory", "stored-workflows", "enabled=true", "trigger_task_type=write_file", "source=promoted_candidate"});
    Expect(stored_enabled_workflows.exit_code == 0,
        "memory stored-workflows should accept enabled/trigger/source filters");
    Expect(stored_enabled_workflows.output.find("write_file_workflow") != std::string::npos,
        "memory stored-workflows should include matching filtered workflows");

    const auto stored_name_filtered_workflows = RunAgentos(workspace, {
        "memory", "stored-workflows", "name_contains=missing"});
    Expect(stored_name_filtered_workflows.exit_code == 0,
        "memory stored-workflows should accept name_contains filters");
    Expect(stored_name_filtered_workflows.output.find("write_file_workflow") == std::string::npos,
        "memory stored-workflows should omit non-matching filtered workflows");

    const auto invalid_stored_filter = RunAgentos(workspace, {
        "memory", "stored-workflows", "enabled=maybe"});
    Expect(invalid_stored_filter.exit_code != 0,
        "memory stored-workflows should reject invalid enabled filter values");
    Expect(invalid_stored_filter.output.find("enabled must be true or false") != std::string::npos,
        "memory stored-workflows should explain invalid enabled filter values");

    const auto disable_workflow = RunAgentos(workspace, {
        "memory", "set-workflow-enabled", "write_file_workflow", "enabled=false"});
    Expect(disable_workflow.exit_code == 0, "memory set-workflow-enabled should disable existing workflows");
    Expect(disable_workflow.output.find("enabled=false") != std::string::npos,
        "memory set-workflow-enabled should print the updated disabled workflow");

    const auto enable_workflow = RunAgentos(workspace, {
        "memory", "set-workflow-enabled", "write_file_workflow", "enabled=true"});
    Expect(enable_workflow.exit_code == 0, "memory set-workflow-enabled should enable existing workflows");
    Expect(enable_workflow.output.find("enabled=true") != std::string::npos,
        "memory set-workflow-enabled should print the updated enabled workflow");

    const auto invalid_enable_workflow = RunAgentos(workspace, {
        "memory", "set-workflow-enabled", "write_file_workflow", "enabled=maybe"});
    Expect(invalid_enable_workflow.exit_code != 0,
        "memory set-workflow-enabled should reject invalid enabled values");
    Expect(invalid_enable_workflow.output.find("enabled must be true or false") != std::string::npos,
        "memory set-workflow-enabled should explain invalid enabled values");

    const auto validate_workflows = RunAgentos(workspace, {"memory", "validate-workflows"});
    Expect(validate_workflows.exit_code == 0, "memory validate-workflows should pass for valid workflow definitions");
    Expect(validate_workflows.output.find("valid=true") != std::string::npos,
        "memory validate-workflows should report valid workflow definitions");

    const auto explain_matching = RunAgentos(workspace, {
        "memory", "explain-workflow", "write_file_workflow", "task_type=write_file", "branch=release/2026.04"});
    Expect(explain_matching.exit_code == 0, "memory explain-workflow should succeed for existing workflows");
    Expect(explain_matching.output.find("applicable=true") != std::string::npos,
        "memory explain-workflow should report applicable=true for matching inputs");
    Expect(explain_matching.output.find("field=input_regex") != std::string::npos,
        "memory explain-workflow should include condition-level checks");

    const auto explain_non_matching = RunAgentos(workspace, {
        "memory", "explain-workflow", "write_file_workflow", "task_type=write_file", "branch=feature/demo"});
    Expect(explain_non_matching.exit_code == 0, "memory explain-workflow should not fail just because inputs do not match");
    Expect(explain_non_matching.output.find("applicable=false") != std::string::npos,
        "memory explain-workflow should report applicable=false for non-matching inputs");
    Expect(explain_non_matching.output.find("matched=false") != std::string::npos,
        "memory explain-workflow should explain which condition failed");

    const auto update_workflow = RunAgentos(workspace, {
        "memory", "update-workflow", "write_file_workflow",
        "required_inputs=path,content",
        "input_regex=branch=hotfix/.*",
        "input_bool=approved=true"});
    Expect(update_workflow.exit_code == 0, "memory update-workflow should update existing workflows");
    Expect(update_workflow.output.find("required_inputs=path,content") != std::string::npos,
        "memory update-workflow should print updated required inputs");
    Expect(update_workflow.output.find("input_regex=branch=hotfix/.*") != std::string::npos,
        "memory update-workflow should print updated regex conditions");
    Expect(update_workflow.output.find("input_bool=approved=true") != std::string::npos,
        "memory update-workflow should print updated boolean conditions");

    const auto explain_updated = RunAgentos(workspace, {
        "memory", "explain-workflow", "write_file_workflow",
        "task_type=write_file", "path=memory_storage/workflow_c.txt", "content=c",
        "branch=hotfix/urgent", "approved=true"});
    Expect(explain_updated.exit_code == 0, "memory explain-workflow should succeed after update-workflow");
    Expect(explain_updated.output.find("applicable=true") != std::string::npos,
        "updated workflow conditions should be used by explain-workflow");

    const auto rename_and_clear_workflow = RunAgentos(workspace, {
        "memory", "update-workflow", "write_file_workflow",
        "new_name=renamed_write_file_workflow",
        "input_regex=",
        "input_bool="});
    Expect(rename_and_clear_workflow.exit_code == 0,
        "memory update-workflow should rename workflows and clear list-valued conditions");
    Expect(rename_and_clear_workflow.output.find("renamed_write_file_workflow") != std::string::npos,
        "memory update-workflow should print the renamed workflow");
    Expect(rename_and_clear_workflow.output.find("input_regex= input_any=") != std::string::npos,
        "memory update-workflow should print cleared regex conditions");
    Expect(rename_and_clear_workflow.output.find("input_bool= input_regex=") != std::string::npos,
        "memory update-workflow should print cleared boolean conditions");

    const auto show_old_name_after_rename = RunAgentos(workspace, {"memory", "show-workflow", "write_file_workflow"});
    Expect(show_old_name_after_rename.exit_code != 0,
        "memory update-workflow rename should remove the old workflow name");
    Expect(show_old_name_after_rename.output.find("workflow not found: write_file_workflow") != std::string::npos,
        "show-workflow should explain old workflow name lookup failures after rename");

    const auto explain_renamed = RunAgentos(workspace, {
        "memory", "explain-workflow", "renamed_write_file_workflow",
        "task_type=write_file", "path=memory_storage/workflow_c.txt", "content=c"});
    Expect(explain_renamed.exit_code == 0, "memory explain-workflow should succeed for renamed workflows");
    Expect(explain_renamed.output.find("applicable=true") != std::string::npos,
        "cleared workflow conditions should no longer be required for applicability");

    const auto clone_workflow = RunAgentos(workspace, {
        "memory", "clone-workflow", "renamed_write_file_workflow", "new_name=cloned_write_file_workflow"});
    Expect(clone_workflow.exit_code == 0, "memory clone-workflow should clone existing workflows");
    Expect(clone_workflow.output.find("cloned_write_file_workflow") != std::string::npos,
        "memory clone-workflow should print the cloned workflow");
    Expect(clone_workflow.output.find("source=cloned_workflow") != std::string::npos,
        "memory clone-workflow should mark cloned workflow source");

    const auto show_cloned_workflow = RunAgentos(workspace, {"memory", "show-workflow", "cloned_write_file_workflow"});
    Expect(show_cloned_workflow.exit_code == 0, "memory show-workflow should find cloned workflows");
    Expect(show_cloned_workflow.output.find("steps=file_write") != std::string::npos,
        "cloned workflow should preserve ordered steps");

    const auto clone_existing_workflow = RunAgentos(workspace, {
        "memory", "clone-workflow", "renamed_write_file_workflow", "new_name=cloned_write_file_workflow"});
    Expect(clone_existing_workflow.exit_code != 0, "memory clone-workflow should reject existing target names");
    Expect(clone_existing_workflow.output.find("workflow already exists: cloned_write_file_workflow") !=
               std::string::npos,
        "memory clone-workflow should explain duplicate target failures");

    const auto invalid_update = RunAgentos(workspace, {
        "memory", "update-workflow", "renamed_write_file_workflow", "input_expr=equals:mode=workflow && ("});
    Expect(invalid_update.exit_code != 0, "memory update-workflow should reject invalid workflow definitions");
    Expect(invalid_update.output.find("field=input_expr") != std::string::npos,
        "invalid update-workflow output should name invalid fields");

    const auto invalid_promote = RunAgentos(workspace, {
        "memory", "promote-workflow", "write_file_workflow", "input_expr=equals:mode=workflow && ("});
    Expect(invalid_promote.exit_code != 0, "memory promote-workflow should reject invalid input_expr conditions");
    Expect(invalid_promote.output.find("field=input_expr") != std::string::npos,
        "invalid promote-workflow output should name the invalid input_expr field");

    {
        std::ofstream workflow_file(workspace / "runtime" / "memory" / "workflows.tsv", std::ios::app | std::ios::binary);
        workflow_file
            << "bad_workflow" << '\t'
            << "1" << '\t'
            << "write_file" << '\t'
            << "file_write" << '\t'
            << "manual" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "equals:mode=workflow&&("
            << '\n';
    }

    const auto invalid_validate = RunAgentos(workspace, {"memory", "validate-workflows"});
    Expect(invalid_validate.exit_code != 0, "memory validate-workflows should fail for invalid persisted workflow definitions");
    Expect(invalid_validate.output.find("workflow=bad_workflow") != std::string::npos,
        "memory validate-workflows should identify invalid workflow definitions");
    Expect(invalid_validate.output.find("field=input_expr") != std::string::npos,
        "memory validate-workflows should identify invalid workflow expression fields");

    const auto remove_cloned_workflow = RunAgentos(workspace, {"memory", "remove-workflow", "cloned_write_file_workflow"});
    Expect(remove_cloned_workflow.exit_code == 0, "memory remove-workflow should remove cloned workflows");
    Expect(remove_cloned_workflow.output.find("removed cloned_write_file_workflow") != std::string::npos,
        "memory remove-workflow should report removed cloned workflows");

    const auto remove_workflow = RunAgentos(workspace, {"memory", "remove-workflow", "renamed_write_file_workflow"});
    Expect(remove_workflow.exit_code == 0, "memory remove-workflow should remove existing workflows");
    Expect(remove_workflow.output.find("removed renamed_write_file_workflow") != std::string::npos,
        "memory remove-workflow should report removed workflows");

    const auto show_removed_workflow = RunAgentos(workspace, {"memory", "show-workflow", "renamed_write_file_workflow"});
    Expect(show_removed_workflow.exit_code != 0, "removed workflows should no longer be inspectable");
    Expect(show_removed_workflow.output.find("workflow not found: renamed_write_file_workflow") != std::string::npos,
        "show-workflow should explain removed workflow lookup failures");

    const auto remove_missing_workflow = RunAgentos(workspace, {"memory", "remove-workflow", "renamed_write_file_workflow"});
    Expect(remove_missing_workflow.exit_code != 0, "memory remove-workflow should fail for missing workflows");
    Expect(remove_missing_workflow.output.find("not_found renamed_write_file_workflow") != std::string::npos,
        "memory remove-workflow should report missing workflows");

    const auto storage = RunAgentos(workspace, {"storage", "status"});
    Expect(storage.exit_code == 0, "storage status command should succeed");
    Expect(storage.output.find("decision_id=ADR-STORAGE-001") != std::string::npos,
        "storage status should print the storage decision record id");
    Expect(storage.output.find("backend=tsv") != std::string::npos, "storage status should report TSV backend");
    Expect(storage.output.find("target_backend=sqlite") != std::string::npos,
        "storage status should report the deferred target backend");
    Expect(storage.output.find("migration_boundary=") != std::string::npos,
        "storage status should report the SQLite migration boundary");
    Expect(storage.output.find("required_interface=") != std::string::npos,
        "storage status should report required backend interface capabilities");
    Expect(storage.output.find("runtime/storage_manifest.tsv") != std::string::npos,
        "storage status should list runtime storage manifest");
    Expect(storage.output.find("runtime format=manifest.tsv version=1 path=runtime/storage_manifest.tsv exists=true") != std::string::npos,
        "storage status should report manifest existence");
    Expect(storage.output.find("bytes=") != std::string::npos && storage.output.find("lines=") != std::string::npos,
        "storage status should report file size and line counts");

    const auto storage_verify = RunAgentos(workspace, {"storage", "verify"});
    Expect(storage_verify.exit_code == 0, "storage verify should succeed in diagnostic mode");
    Expect(storage_verify.output.find("storage_verify component=runtime path=runtime/storage_manifest.tsv exists=true regular=true") != std::string::npos,
        "storage verify should report per-component file existence");
    Expect(storage_verify.output.find("storage_verify_summary") != std::string::npos,
        "storage verify should print a summary");

    const auto strict_workspace = FreshWorkspace("storage_strict_verify");
    const auto strict_status = RunAgentos(strict_workspace, {"storage", "status"});
    Expect(strict_status.exit_code == 0, "storage status should initialize a fresh storage manifest");
    const auto strict_verify = RunAgentos(strict_workspace, {"storage", "verify", "strict=true"});
    Expect(strict_verify.exit_code != 0, "storage verify strict=true should fail when manifested files are missing");
    Expect(strict_verify.output.find("strict=true") != std::string::npos &&
            strict_verify.output.find("ok=false") != std::string::npos,
        "storage verify strict=true should explain failed completeness checks");

    const auto backup_source = workspace / "storage_verify_source";
    std::filesystem::create_directories(backup_source / "runtime");
    {
        std::ofstream manifest(backup_source / "runtime" / "storage_manifest.tsv", std::ios::binary);
        manifest
            << "runtime\tmanifest.tsv\t1\truntime/storage_manifest.tsv\n"
            << "auth_sessions\ttsv\t1\truntime/auth_sessions.tsv\n";
    }
    {
        std::ofstream sessions(backup_source / "runtime" / "auth_sessions.tsv", std::ios::binary);
        sessions << "session\tdata\n";
    }
    const auto backup_verify = RunAgentos(workspace, {
        "storage", "verify", "src=" + backup_source.string(), "strict=true"});
    Expect(backup_verify.exit_code == 0, "storage verify src=... strict=true should validate complete backup sources");
    Expect(backup_verify.output.find("root=\"" + backup_source.string() + "\"") != std::string::npos,
        "storage verify src=... should report the verified root");
    std::filesystem::remove(backup_source / "runtime" / "auth_sessions.tsv");
    const auto incomplete_backup_verify = RunAgentos(workspace, {
        "storage", "verify", "src=" + backup_source.string(), "strict=true"});
    Expect(incomplete_backup_verify.exit_code != 0,
        "storage verify src=... strict=true should fail when backup source files are missing");
    Expect(incomplete_backup_verify.output.find("missing=1") != std::string::npos,
        "storage verify src=... should report missing backup source files");

    const auto import_source = workspace / "storage_import_source";
    std::filesystem::create_directories(import_source / "runtime");
    {
        std::ofstream manifest(import_source / "runtime" / "storage_manifest.tsv", std::ios::binary);
        manifest
            << "runtime\tmanifest.tsv\t1\truntime/storage_manifest.tsv\n"
            << "auth_sessions\ttsv\t1\truntime/auth_sessions.tsv\n";
    }
    {
        std::ofstream imported(import_source / "runtime" / "auth_sessions.tsv", std::ios::binary);
        imported << "imported-session\n";
    }
    {
        std::ofstream existing(workspace / "runtime" / "auth_sessions.tsv", std::ios::binary);
        existing << "pre-import-session\n";
    }
    const auto import_result = RunAgentos(workspace, {"storage", "import", "src=" + import_source.string()});
    Expect(import_result.exit_code == 0, "storage import should succeed with a valid manifest source");
    Expect(import_result.output.find("backed_up_files=") != std::string::npos &&
            import_result.output.find("backup=") != std::string::npos,
        "storage import should report backup metadata for overwritten managed files");
    Expect(ReadTextFile(workspace / "runtime" / "auth_sessions.tsv") == "imported-session\n",
        "storage import should overwrite managed files from the source");
    const auto storage_backups = RunAgentos(workspace, {"storage", "backups"});
    Expect(storage_backups.exit_code == 0, "storage backups should succeed after import backup creation");
    Expect(storage_backups.output.find("storage_backup name=import-") != std::string::npos,
        "storage backups should list import backup directories");
    Expect(storage_backups.output.find("path=runtime/.import_backups/import-") != std::string::npos,
        "storage backups should report scriptable relative backup paths");
    Expect(storage_backups.output.find("files=") != std::string::npos &&
            storage_backups.output.find("bytes=") != std::string::npos,
        "storage backups should report backup file and byte counts");
    Expect(storage_backups.output.find("storage_backups_summary count=") != std::string::npos,
        "storage backups should print an aggregate summary");
    const auto backup_name = ExtractTokenValue(storage_backups.output, "storage_backup name=");
    Expect(!backup_name.empty(), "storage backups output should expose a reusable backup name");
    {
        std::ofstream changed(workspace / "runtime" / "auth_sessions.tsv", std::ios::binary);
        changed << "post-import-change\n";
    }
    const auto restore_backup = RunAgentos(workspace, {"storage", "restore-backup", "name=" + backup_name});
    Expect(restore_backup.exit_code == 0, "storage restore-backup should restore a listed import backup");
    Expect(restore_backup.output.find("restored_files=") != std::string::npos &&
            restore_backup.output.find("backup=" + backup_name) != std::string::npos,
        "storage restore-backup should report restored files and source backup");
    Expect(ReadTextFile(workspace / "runtime" / "auth_sessions.tsv") == "pre-import-session\n",
        "storage restore-backup should restore backed up managed files");

    const auto missing_restore = RunAgentos(workspace, {"storage", "restore-backup", "name=missing-backup"});
    Expect(missing_restore.exit_code != 0, "storage restore-backup should fail for missing backups");
    Expect(missing_restore.output.find("backup not found: missing-backup") != std::string::npos,
        "storage restore-backup should explain missing backup lookup failures");

    const auto unsafe_restore = RunAgentos(workspace, {"storage", "restore-backup", "name=..\\outside"});
    Expect(unsafe_restore.exit_code != 0, "storage restore-backup should reject path-like backup names");
    Expect(unsafe_restore.output.find("name must be a backup directory name") != std::string::npos,
        "storage restore-backup should explain invalid backup names");

    const auto empty_backups_workspace = FreshWorkspace("storage_backups_empty");
    const auto empty_backups = RunAgentos(empty_backups_workspace, {"storage", "backups"});
    Expect(empty_backups.exit_code == 0, "storage backups should succeed without any backup directories");
    Expect(empty_backups.output.find("storage_backups_summary count=0 files=0 bytes=0") != std::string::npos,
        "storage backups should report an empty summary for a fresh workspace");

    const auto committed_txn = workspace / "runtime" / ".transactions" / "cli-recover";
    std::filesystem::create_directories(committed_txn);
    {
        std::ofstream data(committed_txn / "file_0.data", std::ios::binary);
        data << "recovered-by-cli\n";
    }
    {
        std::ofstream prepare(committed_txn / "prepare.tsv", std::ios::binary);
        prepare << (workspace / "runtime" / "recovered.tsv").generic_string() << '\t' << "file_0.data\n";
    }
    {
        std::ofstream commit(committed_txn / "commit", std::ios::binary);
        commit << "committed\n";
    }
    const auto rolled_back_txn = workspace / "runtime" / ".transactions" / "cli-rollback";
    std::filesystem::create_directories(rolled_back_txn);
    {
        std::ofstream data(rolled_back_txn / "file_0.data", std::ios::binary);
        data << "should-not-apply\n";
    }
    {
        std::ofstream prepare(rolled_back_txn / "prepare.tsv", std::ios::binary);
        prepare << (workspace / "runtime" / "rolled_back.tsv").generic_string() << '\t' << "file_0.data\n";
    }
    const auto corrupt_txn = workspace / "runtime" / ".transactions" / "cli-corrupt";
    std::filesystem::create_directories(corrupt_txn);
    {
        std::ofstream prepare(corrupt_txn / "prepare.tsv", std::ios::binary);
        prepare << (workspace / "runtime" / "corrupt_recovered.tsv").generic_string() << '\t' << "missing.data\n";
    }
    {
        std::ofstream commit(corrupt_txn / "commit", std::ios::binary);
        commit << "committed\n";
    }

    const auto recover = RunAgentos(workspace, {"storage", "recover"});
    Expect(recover.exit_code == 0, "storage recover command should succeed");
    Expect(recover.output.find("committed_replayed=1") != std::string::npos,
        "storage recover should replay committed transaction markers");
    Expect(recover.output.find("rolled_back=1") != std::string::npos,
        "storage recover should roll back uncommitted prepared transactions");
    Expect(recover.output.find("failed=1") != std::string::npos,
        "storage recover should report corrupt committed transaction failures");
    Expect(ReadTextFile(workspace / "runtime" / "recovered.tsv") == "recovered-by-cli\n",
        "storage recover should apply committed transaction data");
    Expect(!std::filesystem::exists(workspace / "runtime" / "rolled_back.tsv"),
        "storage recover should not apply uncommitted transaction data");
    Expect(!std::filesystem::exists(workspace / "runtime" / "corrupt_recovered.tsv"),
        "storage recover should not apply corrupt committed transaction data");
}

void TestTrustCommands() {
    const auto workspace = FreshWorkspace("trust");

    const auto identity_add = RunAgentos(workspace, {
        "trust", "identity-add", "identity=phone", "user=local-user", "label=dev-phone"});
    Expect(identity_add.exit_code == 0, "trust identity-add should succeed");
    Expect(identity_add.output.find("phone user=local-user label=dev-phone") != std::string::npos,
        "trust identity-add should print the saved identity");

    const auto identities = RunAgentos(workspace, {"trust", "identities"});
    Expect(identities.exit_code == 0, "trust identities should succeed");
    Expect(identities.output.find("phone user=local-user label=dev-phone") != std::string::npos,
        "trust identities should list the saved identity");

    const auto pair = RunAgentos(workspace, {
        "trust", "pair", "identity=phone", "device=device1", "label=dev-phone", "permissions=task.submit"});
    Expect(pair.exit_code == 0, "trust pair should succeed");
    Expect(pair.output.find("phone device=device1") != std::string::npos,
        "trust pair should print the paired peer");

    const auto list = RunAgentos(workspace, {"trust", "list"});
    Expect(list.exit_code == 0, "trust list should succeed");
    Expect(list.output.find("permissions=task.submit") != std::string::npos,
        "trust list should include paired permissions");
    Expect(list.output.find("paired_epoch_ms=") != std::string::npos
            && list.output.find("last_seen_epoch_ms=") != std::string::npos,
        "trust list should include device lifecycle timestamps");

    const auto device_label = RunAgentos(workspace, {
        "trust", "device-label", "identity=phone", "device=device1", "label=renamed-phone"});
    Expect(device_label.exit_code == 0, "trust device-label should succeed");
    const auto device_seen = RunAgentos(workspace, {"trust", "device-seen", "identity=phone", "device=device1"});
    Expect(device_seen.exit_code == 0, "trust device-seen should succeed");
    const auto block = RunAgentos(workspace, {"trust", "block", "identity=phone", "device=device1"});
    Expect(block.exit_code == 0, "trust block should succeed");
    const auto unblock = RunAgentos(workspace, {"trust", "unblock", "identity=phone", "device=device1"});
    Expect(unblock.exit_code == 0, "trust unblock should succeed");
    const auto list_after_lifecycle = RunAgentos(workspace, {"trust", "list"});
    Expect(list_after_lifecycle.output.find("label=renamed-phone") != std::string::npos,
        "trust list should include renamed device label");
    Expect(list_after_lifecycle.output.find("trust=paired") != std::string::npos,
        "trust unblock should restore paired trust state");

    const auto device_show = RunAgentos(workspace, {"trust", "device-show", "identity=phone", "device=device1"});
    Expect(device_show.exit_code == 0, "trust device-show should succeed for existing paired devices");
    Expect(device_show.output.find("phone device=device1") != std::string::npos,
        "trust device-show should print the requested device");
    Expect(device_show.output.find("label=renamed-phone") != std::string::npos,
        "trust device-show should print device lifecycle metadata");

    const auto device_show_missing = RunAgentos(workspace, {"trust", "device-show", "identity=phone", "device=missing"});
    Expect(device_show_missing.exit_code != 0, "trust device-show should fail for missing devices");
    Expect(device_show_missing.output.find("not_found phone device=missing") != std::string::npos,
        "trust device-show should report missing devices");

    const auto invite_create = RunAgentos(workspace, {
        "trust", "invite-create", "identity=tablet", "device=device2", "label=dev-tablet",
        "user=local-user", "identity_label=tablet-identity", "permissions=task.submit", "ttl_seconds=600"});
    Expect(invite_create.exit_code == 0, "trust invite-create should succeed");
    Expect(invite_create.output.find("invite token=") != std::string::npos,
        "trust invite-create should print an invite token");
    const auto token_prefix = std::string("token=");
    const auto token_start = invite_create.output.find(token_prefix);
    std::string invite_token;
    if (token_start != std::string::npos) {
        const auto value_start = token_start + token_prefix.size();
        const auto value_end = invite_create.output.find(' ', value_start);
        invite_token = invite_create.output.substr(value_start, value_end - value_start);
    }
    Expect(!invite_token.empty(), "trust invite-create token should be parseable");

    const auto invites = RunAgentos(workspace, {"trust", "invites"});
    Expect(invites.exit_code == 0, "trust invites should succeed");
    Expect(invites.output.find(invite_token) != std::string::npos,
        "trust invites should list active invite tokens");

    const auto invite_accept = RunAgentos(workspace, {"trust", "invite-accept", "token=" + invite_token});
    Expect(invite_accept.exit_code == 0, "trust invite-accept should succeed");
    Expect(invite_accept.output.find("tablet device=device2") != std::string::npos,
        "trust invite-accept should pair the invited device");

    const auto invite_accept_again = RunAgentos(workspace, {"trust", "invite-accept", "token=" + invite_token});
    Expect(invite_accept_again.exit_code != 0, "trust invite-accept should reject consumed tokens");

    const auto role_set = RunAgentos(workspace, {
        "trust", "role-set", "role=reader", "permissions=filesystem.read"});
    Expect(role_set.exit_code == 0, "trust role-set should succeed");
    Expect(role_set.output.find("role reader permissions=filesystem.read") != std::string::npos,
        "trust role-set should print saved role permissions");

    const auto user_role = RunAgentos(workspace, {
        "trust", "user-role", "user=alice", "roles=reader"});
    Expect(user_role.exit_code == 0, "trust user-role should succeed");
    Expect(user_role.output.find("user alice roles=reader") != std::string::npos,
        "trust user-role should print saved user role assignment");

    const auto roles = RunAgentos(workspace, {"trust", "roles"});
    Expect(roles.exit_code == 0, "trust roles should succeed");
    Expect(roles.output.find("role reader permissions=filesystem.read") != std::string::npos,
        "trust roles should list persisted role definitions");
    Expect(roles.output.find("user alice roles=reader") != std::string::npos,
        "trust roles should list persisted user role assignments");

    const auto role_show = RunAgentos(workspace, {"trust", "role-show", "role=reader"});
    Expect(role_show.exit_code == 0, "trust role-show should succeed for existing roles");
    Expect(role_show.output.find("role reader permissions=filesystem.read") != std::string::npos,
        "trust role-show should print the requested role");

    const auto role_show_missing = RunAgentos(workspace, {"trust", "role-show", "role=missing"});
    Expect(role_show_missing.exit_code != 0, "trust role-show should fail for missing roles");
    Expect(role_show_missing.output.find("not_found role missing") != std::string::npos,
        "trust role-show should report missing roles");

    const auto user_role_show = RunAgentos(workspace, {"trust", "user-role-show", "user=alice"});
    Expect(user_role_show.exit_code == 0, "trust user-role-show should succeed for existing assignments");
    Expect(user_role_show.output.find("user alice roles=reader") != std::string::npos,
        "trust user-role-show should print the requested user assignment");

    const auto user_role_show_missing = RunAgentos(workspace, {"trust", "user-role-show", "user=bob"});
    Expect(user_role_show_missing.exit_code != 0, "trust user-role-show should fail for missing assignments");
    Expect(user_role_show_missing.output.find("not_found user bob") != std::string::npos,
        "trust user-role-show should report missing assignments");

    const auto approval_request = RunAgentos(workspace, {
        "trust", "approval-request", "subject=critical-write", "reason=operator-check", "requested_by=alice"});
    Expect(approval_request.exit_code == 0, "trust approval-request should succeed");
    Expect(approval_request.output.find("status=pending") != std::string::npos,
        "trust approval-request should create pending approvals");
    const auto approval_prefix = std::string("approval ");
    const auto approval_start = approval_request.output.find(approval_prefix);
    std::string approval_id;
    if (approval_start != std::string::npos) {
        const auto value_start = approval_start + approval_prefix.size();
        const auto value_end = approval_request.output.find(' ', value_start);
        approval_id = approval_request.output.substr(value_start, value_end - value_start);
    }
    Expect(!approval_id.empty(), "trust approval-request id should be parseable");

    const auto approval_approve = RunAgentos(workspace, {
        "trust", "approval-approve", "approval=" + approval_id, "approved_by=admin"});
    Expect(approval_approve.exit_code == 0, "trust approval-approve should succeed");
    Expect(approval_approve.output.find("status=approved") != std::string::npos,
        "trust approval-approve should mark approvals approved");

    const auto approval_show = RunAgentos(workspace, {"trust", "approval-show", "approval=" + approval_id});
    Expect(approval_show.exit_code == 0, "trust approval-show should succeed for existing approvals");
    Expect(approval_show.output.find("approval " + approval_id) != std::string::npos,
        "trust approval-show should print the requested approval");
    Expect(approval_show.output.find("status=approved") != std::string::npos,
        "trust approval-show should print approval status");

    const auto approval_show_missing = RunAgentos(workspace, {"trust", "approval-show", "approval=missing-approval"});
    Expect(approval_show_missing.exit_code != 0, "trust approval-show should fail for missing approvals");
    Expect(approval_show_missing.output.find("not_found approval missing-approval") != std::string::npos,
        "trust approval-show should report missing approvals");

    const auto approvals = RunAgentos(workspace, {"trust", "approvals"});
    Expect(approvals.exit_code == 0, "trust approvals should succeed");
    Expect(approvals.output.find(approval_id) != std::string::npos,
        "trust approvals should list persisted approvals");

    const auto approval_revoke = RunAgentos(workspace, {
        "trust", "approval-revoke", "approval=" + approval_id, "approved_by=admin"});
    Expect(approval_revoke.exit_code == 0, "trust approval-revoke should succeed");
    Expect(approval_revoke.output.find("status=revoked") != std::string::npos,
        "trust approval-revoke should mark approvals revoked");

    {
        std::ofstream readme(workspace / "README.md", std::ios::binary);
        readme << "role catalog fixture\n";
    }
    const auto role_allowed_read = RunAgentos(workspace, {
        "run", "read_file", "user=alice", "path=README.md"});
    Expect(role_allowed_read.exit_code == 0, "role catalog should allow user role filesystem.read");

    const auto role_denied_write = RunAgentos(workspace, {
        "run", "write_file", "user=alice", "path=runtime/role-denied.txt", "content=nope", "idempotency_key=role-denied"});
    Expect(role_denied_write.exit_code != 0, "role catalog should deny missing user role filesystem.write");
    Expect(role_denied_write.output.find("filesystem.write") != std::string::npos,
        "role catalog denial should name the missing write permission");

    const auto user_role_remove = RunAgentos(workspace, {
        "trust", "user-role-remove", "user=alice"});
    Expect(user_role_remove.exit_code == 0, "trust user-role-remove should succeed for existing assignments");
    const auto read_after_user_remove = RunAgentos(workspace, {
        "run", "read_file", "user=alice", "path=README.md"});
    Expect(read_after_user_remove.exit_code == 0,
        "removing user roles should fall back to unconstrained local task behavior");

    const auto user_role_restore = RunAgentos(workspace, {
        "trust", "user-role", "user=alice", "roles=reader"});
    Expect(user_role_restore.exit_code == 0, "trust user-role should allow restoring a user assignment");

    const auto role_remove = RunAgentos(workspace, {
        "trust", "role-remove", "role=reader"});
    Expect(role_remove.exit_code == 0, "trust role-remove should remove existing roles");
    const auto roles_after_remove = RunAgentos(workspace, {"trust", "roles"});
    Expect(roles_after_remove.output.find("role reader permissions=filesystem.read") == std::string::npos,
        "trust roles should not list removed roles");
    Expect(roles_after_remove.output.find("user alice roles=reader") == std::string::npos,
        "removing a role should clean user assignments that referenced it");
}

void TestScheduleCommands() {
    const auto workspace = FreshWorkspace("schedule");

    const auto add = RunAgentos(workspace, {
        "schedule", "add", "id=cli-schedule", "task=write_file", "due=now",
        "path=runtime/scheduled.txt", "content=hello"});
    Expect(add.exit_code == 0, "schedule add should succeed");
    Expect(add.output.find("cli-schedule enabled=true") != std::string::npos,
        "schedule add should print the stored task");

    const auto list = RunAgentos(workspace, {"schedule", "list"});
    Expect(list.exit_code == 0, "schedule list should succeed");
    Expect(list.output.find("cli-schedule") != std::string::npos, "schedule list should include the added task");

    const auto add_cron = RunAgentos(workspace, {
        "schedule", "add", "id=cli-cron", "task=write_file", "cron=* * * * *",
        "path=runtime/cron.txt", "content=cron"});
    Expect(add_cron.exit_code == 0, "schedule add should accept a five-field cron expression");
    Expect(add_cron.output.find("cron=\"* * * * *\"") != std::string::npos,
        "schedule add should print the configured cron expression");

    const auto add_cron_alias = RunAgentos(workspace, {
        "schedule", "add", "id=cli-cron-alias", "task=write_file", "cron=@hourly",
        "path=runtime/cron-alias.txt", "content=cron-alias"});
    Expect(add_cron_alias.exit_code == 0, "schedule add should accept @hourly cron alias");
    Expect(add_cron_alias.output.find("cron=\"@hourly\"") != std::string::npos,
        "schedule add should print the configured cron alias");

    const auto invalid_cron = RunAgentos(workspace, {
        "schedule", "add", "id=cli-bad-cron", "task=write_file", "cron=*/0 * * * *",
        "path=runtime/bad-cron.txt", "content=bad"});
    Expect(invalid_cron.exit_code != 0, "schedule add should reject invalid cron expressions");
    Expect(invalid_cron.output.find("cron expression must use five fields") != std::string::npos,
        "schedule add should explain invalid cron expressions");

    const auto run_due = RunAgentos(workspace, {"schedule", "run-due"});
    Expect(run_due.exit_code == 0, "schedule run-due should succeed for a valid write task");
    Expect(run_due.output.find("cli-schedule success=true") != std::string::npos,
        "schedule run-due should report successful execution");
    Expect(std::filesystem::exists(workspace / "runtime" / "scheduled.txt"),
        "schedule run-due should create the scheduled output file");

    const auto history = RunAgentos(workspace, {"schedule", "history"});
    Expect(history.exit_code == 0, "schedule history should succeed");
    Expect(history.output.find("cli-schedule") != std::string::npos,
        "schedule history should include the executed task");

    const auto remove = RunAgentos(workspace, {"schedule", "remove", "id=cli-schedule"});
    Expect(remove.exit_code == 0, "schedule remove should succeed");
    Expect(remove.output.find("removed cli-schedule") != std::string::npos,
        "schedule remove should confirm deletion");
}

void TestSubagentsCommand() {
    const auto workspace = FreshWorkspace("subagents");
    const auto result = RunAgentos(workspace, {
        "subagents", "run", "agents=local_planner", "mode=sequential", "objective=Plan_the_next_phase"});

    Expect(result.exit_code == 0, "subagents run should succeed for local_planner");
    Expect(result.output.find("success: true") != std::string::npos,
        "subagents run should print a successful result");
    Expect(result.output.find("local_planner") != std::string::npos,
        "subagents run should mention the selected agent");

    const auto roles_result = RunAgentos(workspace, {
        "subagents", "run", "agents=local_planner", "mode=sequential", "roles=local_planner:planner", "objective=Plan_with_role"});
    Expect(roles_result.exit_code == 0, "subagents run should accept explicit role assignments");
    Expect(roles_result.output.find("\"roles\":\"planner\"") != std::string::npos,
        "subagents run output should include explicit role assignment");

    const auto decomposed_result = RunAgentos(workspace, {
        "subagents", "run", "agents=local_planner", "mode=sequential", "roles=local_planner:planner",
        "auto_decompose=true", "decomposition_agent=local_planner", "objective=Coordinate_decomposed_work"});
    Expect(decomposed_result.exit_code == 0, "subagents run should support planner-generated decomposition");
    Expect(decomposed_result.output.find(R"("decomposition_agent":"local_planner")") != std::string::npos,
        "subagents run output should include the decomposition agent");
}

void TestAuthCommands() {
    const auto workspace = FreshWorkspace("auth");

    const auto providers = RunAgentos(workspace, {"auth", "providers"});
    Expect(providers.exit_code == 0, "auth providers should succeed");
    Expect(providers.output.find("openai") != std::string::npos, "auth providers should list openai");
    Expect(providers.output.find("qwen") != std::string::npos, "auth providers should list qwen");

    const auto credential_store = RunAgentos(workspace, {"auth", "credential-store"});
    Expect(credential_store.exit_code == 0, "auth credential-store should succeed");
#ifdef _WIN32
    Expect(credential_store.output.find("backend=windows-credential-manager") != std::string::npos,
        "auth credential-store should report Windows Credential Manager backend on Windows");
#else
    Expect(credential_store.output.find("backend=env-ref-only") != std::string::npos,
        "auth credential-store should report env-ref-only backend");
#endif

    const auto oauth_defaults = RunAgentos(workspace, {"auth", "oauth-defaults"});
    Expect(oauth_defaults.exit_code == 0, "auth oauth-defaults should succeed when any provider has defaults");
    Expect(oauth_defaults.output.find("oauth_defaults provider=gemini") != std::string::npos,
        "auth oauth-defaults should list Gemini defaults");
    Expect(oauth_defaults.output.find("supported=true") != std::string::npos,
        "auth oauth-defaults should mark supported provider defaults");
    Expect(oauth_defaults.output.find("https://oauth2.googleapis.com/token") != std::string::npos,
        "auth oauth-defaults should print token endpoints");
    Expect(oauth_defaults.output.find("cloud-platform") != std::string::npos,
        "auth oauth-defaults should print default scopes");

    std::filesystem::create_directories(workspace / "runtime");
    {
        std::ofstream configured_defaults(workspace / "runtime" / "auth_oauth_providers.tsv", std::ios::binary);
        configured_defaults
            << "# provider\tauthorization_endpoint\ttoken_endpoint\tscopes\n"
            << "gemini\thttps://accounts.example.test/custom-auth\thttps://accounts.example.test/custom-token\topenid,email\n";
    }
    const auto configured_oauth_defaults = RunAgentos(workspace, {"auth", "oauth-defaults", "gemini"});
    Expect(configured_oauth_defaults.exit_code == 0, "auth oauth-defaults should accept repo-local configured defaults");
    Expect(configured_oauth_defaults.output.find("https://accounts.example.test/custom-auth") != std::string::npos,
        "auth oauth-defaults should print configured authorization endpoint");
    Expect(configured_oauth_defaults.output.find("https://accounts.example.test/custom-token") != std::string::npos,
        "auth oauth-defaults should print configured token endpoint");
    Expect(configured_oauth_defaults.output.find("scopes=\"openid,email\"") != std::string::npos,
        "auth oauth-defaults should print configured scopes");
    const auto valid_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate"});
    Expect(valid_oauth_config.exit_code == 0, "auth oauth-config-validate should accept valid repo-local defaults");
    Expect(valid_oauth_config.output.find("valid=true") != std::string::npos,
        "auth oauth-config-validate should report valid configs");

    const auto all_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate", "--all"});
    Expect(all_oauth_config.exit_code == 0, "auth oauth-config-validate --all should succeed for valid configs");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=gemini") != std::string::npos,
        "oauth-config-validate --all should enumerate gemini provider audit row");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=openai") != std::string::npos,
        "oauth-config-validate --all should enumerate openai provider audit row");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=anthropic") != std::string::npos,
        "oauth-config-validate --all should enumerate anthropic provider audit row");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=qwen") != std::string::npos,
        "oauth-config-validate --all should enumerate qwen provider audit row");
    Expect(all_oauth_config.output.find("origin=stub") != std::string::npos,
        "oauth-config-validate --all should mark stubbed providers with origin=stub");
    Expect(all_oauth_config.output.find("origin=config") != std::string::npos,
        "oauth-config-validate --all should mark provider with origin=config when overridden");

    const auto oauth_defaults_origin = RunAgentos(workspace, {"auth", "oauth-defaults", "anthropic"});
    Expect(oauth_defaults_origin.output.find("origin=stub") != std::string::npos,
        "auth oauth-defaults should report origin=stub for unsupported providers");
    Expect(oauth_defaults_origin.output.find("note=") != std::string::npos,
        "auth oauth-defaults should include a note for stubbed providers");

    const auto qwen_oauth_defaults = RunAgentos(workspace, {"auth", "oauth-defaults", "qwen"});
    Expect(qwen_oauth_defaults.exit_code != 0,
        "auth oauth-defaults should return non-zero for providers without built-in OAuth defaults");
    Expect(qwen_oauth_defaults.output.find("oauth_defaults provider=qwen") != std::string::npos,
        "auth oauth-defaults should still print unsupported provider discovery data");
    Expect(qwen_oauth_defaults.output.find("supported=false") != std::string::npos,
        "auth oauth-defaults should mark unsupported provider defaults");

    const auto oauth_start = RunAgentos(workspace, {
        "auth", "oauth-start", "gemini",
        "client_id=test-client",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "profile=oauth-smoke"});
    Expect(oauth_start.exit_code == 0, "auth oauth-start should succeed for browser OAuth providers");
    Expect(oauth_start.output.find("oauth_start provider=gemini profile=oauth-smoke") != std::string::npos,
        "auth oauth-start should print provider and profile");
    Expect(oauth_start.output.find("code_challenge_method=S256") != std::string::npos,
        "auth oauth-start should use PKCE S256");
    Expect(oauth_start.output.find("authorization_url=") != std::string::npos,
        "auth oauth-start should print authorization URL");
    Expect(oauth_start.output.find("https://accounts.example.test/custom-auth") != std::string::npos,
        "auth oauth-start should use repo-local configured authorization endpoint");
    Expect(oauth_start.output.find("scope=openid%20email") != std::string::npos,
        "auth oauth-start should encode configured scopes in authorization URL");

    {
        std::ofstream invalid_defaults(workspace / "runtime" / "auth_oauth_providers.tsv", std::ios::binary);
        invalid_defaults << "gemini\thttps://accounts.example.test/custom-auth\t\n";
    }
    const auto invalid_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate"});
    Expect(invalid_oauth_config.exit_code != 0, "auth oauth-config-validate should reject invalid repo-local defaults");
    Expect(invalid_oauth_config.output.find("token_endpoint is required") != std::string::npos,
        "auth oauth-config-validate should explain missing token endpoints");

    std::filesystem::remove(workspace / "runtime" / "auth_oauth_providers.tsv");
    const auto default_oauth_start = RunAgentos(workspace, {
        "auth", "oauth-start", "gemini",
        "client_id=test-client",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "profile=oauth-defaults"});
    Expect(default_oauth_start.exit_code == 0, "auth oauth-start should use provider defaults when endpoints are omitted");
    Expect(default_oauth_start.output.find("https://accounts.google.com/o/oauth2/v2/auth") != std::string::npos,
        "auth oauth-start should use Gemini's default authorization endpoint");
    Expect(default_oauth_start.output.find("cloud-platform") != std::string::npos,
        "auth oauth-start should include Gemini's default cloud-platform scope");

    const auto unsupported_oauth = RunAgentos(workspace, {
        "auth", "oauth-start", "qwen",
        "client_id=test-client",
        "authorization_endpoint=https://accounts.example.test/oauth",
        "redirect_uri=http://127.0.0.1:48177/callback"});
    Expect(unsupported_oauth.exit_code != 0, "auth oauth-start should reject providers without browser OAuth support");
    Expect(unsupported_oauth.output.find("provider does not support browser OAuth") != std::string::npos,
        "auth oauth-start should explain unsupported providers");

    const auto oauth_callback = RunAgentos(workspace, {
        "auth", "oauth-callback",
        "callback_url=http://127.0.0.1:48177/callback?code=auth%2Fcode&state=state-123",
        "state=state-123"});
    Expect(oauth_callback.exit_code == 0, "auth oauth-callback should succeed with required inputs");
    Expect(oauth_callback.output.find("oauth_callback success=true code=auth/code") != std::string::npos,
        "auth oauth-callback should decode and print callback code");

    const auto oauth_callback_invalid = RunAgentos(workspace, {
        "auth", "oauth-callback",
        "callback_url=http://127.0.0.1:48177/callback?code=auth-code&state=wrong",
        "state=state-123"});
    Expect(oauth_callback_invalid.exit_code == 0, "auth oauth-callback should report invalid callbacks as data");
    Expect(oauth_callback_invalid.output.find("success=false error=InvalidOAuthState") != std::string::npos,
        "auth oauth-callback should report invalid state");

    const auto token_request = RunAgentos(workspace, {
        "auth", "oauth-token-request",
        "token_endpoint=https://oauth2.example.test/token",
        "client_id=client id",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "code=code/value",
        "code_verifier=verifier"});
    Expect(token_request.exit_code == 0, "auth oauth-token-request should succeed with required inputs");
    Expect(token_request.output.find("content_type=application/x-www-form-urlencoded") != std::string::npos,
        "auth oauth-token-request should report form content type");
    Expect(token_request.output.find("grant_type=authorization_code") != std::string::npos,
        "auth oauth-token-request should include authorization_code grant");
    Expect(token_request.output.find("client_id=client%20id") != std::string::npos,
        "auth oauth-token-request should URL-encode client id");
    Expect(token_request.output.find("code=code%2Fvalue") != std::string::npos,
        "auth oauth-token-request should URL-encode authorization code");

    const auto missing_token_request = RunAgentos(workspace, {"auth", "oauth-token-request", "client_id=test"});
    Expect(missing_token_request.exit_code != 0, "auth oauth-token-request should reject missing required inputs");
    Expect(missing_token_request.output.find("token_endpoint is required") != std::string::npos,
        "auth oauth-token-request should explain missing token endpoint");

    const auto refresh_request = RunAgentos(workspace, {
        "auth", "oauth-refresh-request",
        "token_endpoint=https://oauth2.example.test/token",
        "client_id=client id",
        "refresh_token=refresh/value"});
    Expect(refresh_request.exit_code == 0, "auth oauth-refresh-request should succeed with required inputs");
    Expect(refresh_request.output.find("grant_type=refresh_token") != std::string::npos,
        "auth oauth-refresh-request should include refresh_token grant");
    Expect(refresh_request.output.find("refresh_token=refresh%2Fvalue") != std::string::npos,
        "auth oauth-refresh-request should URL-encode refresh token");

    const auto missing_refresh_request = RunAgentos(workspace, {"auth", "oauth-refresh-request", "client_id=test"});
    Expect(missing_refresh_request.exit_code != 0, "auth oauth-refresh-request should reject missing required inputs");
    Expect(missing_refresh_request.output.find("token_endpoint is required") != std::string::npos,
        "auth oauth-refresh-request should explain missing token endpoint");

#ifdef _WIN32
    const auto old_path = ReadEnvForTest("PATH").value_or("");
    const auto bin_dir = workspace / "bin";
    WriteCurlTokenFixture(bin_dir);
    SetEnvForTest("PATH", bin_dir.string() + PathListSeparatorForTest() + old_path);
    const auto oauth_complete = RunAgentos(workspace, {
        "auth", "oauth-complete", "gemini",
        "callback_url=http://127.0.0.1:48177/callback?code=auth-code&state=state-123",
        "state=state-123",
        "code_verifier=verifier",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "client_id=client-id",
        "profile=cli-oauth",
        "account_label=cli@example.test"});
    Expect(oauth_complete.exit_code == 0, "auth oauth-complete should exchange tokens and persist session on Windows");
    Expect(oauth_complete.output.find("gemini profile=cli-oauth") != std::string::npos,
        "auth oauth-complete should print persisted provider and profile");
    Expect(oauth_complete.output.find("profile=cli-oauth") != std::string::npos,
        "auth oauth-complete should print persisted profile");
    Expect(oauth_complete.output.find("source=agentos") != std::string::npos,
        "auth oauth-complete should persist an AgentOS-managed session");

    const auto oauth_login = RunAgentos(workspace, {
        "auth", "oauth-login", "gemini",
        "callback_url=http://127.0.0.1:48177/callback?code=login-code&state=fixed-login-state",
        "state=fixed-login-state",
        "code_verifier=fixed-login-verifier",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "client_id=client-id",
        "profile=cli-oauth-login",
        "account_label=login@example.test"});
    SetEnvForTest("PATH", old_path);
    Expect(oauth_login.exit_code == 0, "auth oauth-login should complete a callback_url OAuth login on Windows");
    Expect(oauth_login.output.find("oauth_start provider=gemini profile=cli-oauth-login") != std::string::npos,
        "auth oauth-login should print the generated OAuth start details");
    Expect(oauth_login.output.find("oauth_callback success=true code=login-code") != std::string::npos,
        "auth oauth-login should validate and print the OAuth callback");
    Expect(oauth_login.output.find("gemini profile=cli-oauth-login") != std::string::npos,
        "auth oauth-login should print the persisted session");
    Expect(oauth_login.output.find("source=agentos") != std::string::npos,
        "auth oauth-login should persist an AgentOS-managed session");
#endif

    const auto old_qwen_key = ReadEnvForTest("QWEN_API_KEY").value_or("");
    SetEnvForTest("QWEN_API_KEY", "test-qwen-key");
    const auto qwen_login = RunAgentos(workspace, {
        "auth", "login", "qwen", "mode=api-key", "api_key_env=QWEN_API_KEY", "profile=team", "set_default=true"});
    SetEnvForTest("QWEN_API_KEY", old_qwen_key);
    Expect(qwen_login.exit_code == 0, "auth login should persist a Qwen API-key profile and allow setting it as default");

    const auto profiles_after_login = RunAgentos(workspace, {"auth", "profiles", "qwen"});
    Expect(profiles_after_login.exit_code == 0, "auth profiles should list persisted provider profiles after login");
    Expect(profiles_after_login.output.find("auth_profile provider=qwen profile=team default=true mode=api_key") != std::string::npos,
        "auth login set_default=true should mark the new profile as the provider default");

    const auto default_profile = RunAgentos(workspace, {"auth", "default-profile", "qwen", "profile=team"});
    Expect(default_profile.exit_code == 0, "auth default-profile should succeed");
    Expect(default_profile.output.find("qwen default_profile=team") != std::string::npos,
        "auth default-profile should confirm persisted profile");

    const auto profiles = RunAgentos(workspace, {"auth", "profiles", "qwen"});
    Expect(profiles.exit_code == 0, "auth profiles should list persisted provider profiles");
    Expect(profiles.output.find("auth_profile provider=qwen profile=team default=true mode=api_key") != std::string::npos,
        "auth profiles should include provider, profile, default marker, and mode");

    const auto status = RunAgentos(workspace, {"auth", "status", "qwen", "profile=team"});
    Expect(status.exit_code == 0, "auth status should succeed for a registered provider");
    Expect(status.output.find("qwen profile=team authenticated=false") != std::string::npos,
        "auth status should report the requested profile and unauthenticated state");

    const auto default_status = RunAgentos(workspace, {"auth", "status", "qwen"});
    Expect(default_status.exit_code == 0, "auth status should use the provider default profile when no profile is provided");
    Expect(default_status.output.find("qwen profile=team") != std::string::npos,
        "auth status should resolve the default profile set during login");
}

}  // namespace

int main() {
    TestAgentsCommand();
    TestCliSpecsCommand();
    TestSpecNameConflictsAreAudited();
    TestPluginNameConflictsWithExternalCliSpec();
    TestPluginsCommand();
    TestMemoryAndStorageCommands();
    TestTrustCommands();
    TestScheduleCommands();
    TestSubagentsCommand();
    TestAuthCommands();

    if (failures != 0) {
        std::cerr << failures << " cli integration test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_cli_integration_tests passed\n";
    return 0;
}
