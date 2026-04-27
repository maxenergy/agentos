#include "auth/auth_models.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/agents/codex_cli_agent.hpp"
#include "hosts/agents/gemini_agent.hpp"
#include "hosts/agents/anthropic_agent.hpp"
#include "hosts/agents/openai_agent.hpp"
#include "hosts/agents/qwen_agent.hpp"
#include "hosts/cli/cli_host.hpp"
#include "utils/cancellation.hpp"
#include "test_command_fixtures.hpp"

#include <algorithm>
#include <atomic>
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
using agentos::test::ScopedEnvOverride;
using agentos::test::SetEnvForTest;
using agentos::test::WriteGeminiCliFixture;
using agentos::test::WriteGcloudCliFixture;
using agentos::test::WriteCliFixture;

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_agent_provider_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string SlashPath(const std::filesystem::path& path) {
    auto value = path.string();
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

// Records argv to `args_file` and, for any argument starting with `@`,
// appends the dereferenced file contents to `<args_file>.deref`. This
// matches how real curl reads `--data @file` / `-H @file` and lets tests
// inspect both the argv (for leak checks) and the request payload.
void WriteCurlFixture(const std::filesystem::path& bin_dir, const std::filesystem::path& args_file) {
    const auto deref_file = SlashPath(std::filesystem::path(args_file.string() + ".deref"));
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + SlashPath(args_file) + "\"\n"
        "type nul > \"" + deref_file + "\"\n"
        ":next\n"
        "set \"arg=%~1\"\n"
        "if \"%arg%\"==\"\" goto done\n"
        "if \"%arg:~0,1%\"==\"@\" type \"%arg:~1%\" >> \"" + deref_file + "\"\n"
        "shift\n"
        "goto next\n"
        ":done\n"
        "echo {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini response\"}]}}]}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
        ": > '" + deref_file + "'\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in\n"
        "    @*) cat \"${arg#@}\" >> '" + deref_file + "' ;;\n"
        "  esac\n"
        "done\n"
        "printf '%s\\n' '{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini response\"}]}}]}'\n");
#endif
}

void TestGeminiAgentUsesAuthenticatedProviderSession(const std::filesystem::path& workspace) {
    const auto bin_dir = workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = workspace / "curl_args.txt";
    WriteCurlFixture(bin_dir, args_file);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    SetEnvForTest("AGENTOS_TEST_GEMINI_KEY", "test-gemini-secret");

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::GeminiAgent agent(cli_host, credential_broker, profile_store, workspace);

    Expect(!agent.healthy(), "gemini agent should be unhealthy before a session is configured");

    profile_store.set_default(agentos::AuthProviderId::gemini, "team");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::gemini, agentos::AuthMode::api_key, "team"),
        .provider = agentos::AuthProviderId::gemini,
        .mode = agentos::AuthMode::api_key,
        .profile_name = "team",
        .account_label = "gemini-test",
        .managed_by_agentos = true,
        .headless_compatible = true,
        .access_token_ref = token_store.make_env_ref("AGENTOS_TEST_GEMINI_KEY"),
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    Expect(agent.healthy(), "gemini agent should be healthy when curl and a gemini session are available");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "gemini-provider",
        .task_type = "analysis",
        .objective = "Explain provider integration",
        .workspace_path = workspace.string(),
        .timeout_ms = 5000,
    });

    Expect(result.success, "gemini agent should return a successful result from curl");
    Expect(result.summary == "gemini response", "gemini agent should extract the first text response");
    Expect(result.structured_output_json.find("\"content\":\"gemini response\"") != std::string::npos,
        "gemini agent should expose normalized content");
    Expect(result.structured_output_json.find("\"schema_version\":\"agent_result.v1\"") != std::string::npos,
        "gemini agent should expose the normalized agent result schema");
    Expect(result.structured_output_json.find("\"provider_metadata\"") != std::string::npos,
        "gemini agent should expose provider metadata in normalized output");
    Expect(result.structured_output_json.find("\"metrics\"") != std::string::npos,
        "gemini agent should expose normalized metrics");
    Expect(result.structured_output_json.find("\"profile\":\"team\"") != std::string::npos,
        "gemini agent should use the configured default profile");
    Expect(result.structured_output_json.find("test-gemini-secret") == std::string::npos,
        "gemini agent structured output should redact the API key");

    const auto args = ReadText(args_file);
    const auto deref = ReadText(std::filesystem::path(args_file.string() + ".deref"));
    Expect(args.find("generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent") != std::string::npos,
        "gemini agent should call the generateContent endpoint");
    Expect(deref.find("x-goog-api-key: test-gemini-secret") != std::string::npos,
        "gemini agent should authenticate API-key sessions with x-goog-api-key (in deref'd headers file)");
    Expect(deref.find("Explain provider integration") != std::string::npos,
        "gemini agent should send the task objective in the request body (deref'd)");
    // Phase 1.1 leak guard: API key must NOT appear on argv.
    Expect(args.find("test-gemini-secret") == std::string::npos,
        "gemini agent must NOT pass API key as argv");
    Expect(args.find("@") != std::string::npos,
        "gemini agent must reference body/header via curl @file syntax");
}

void TestGeminiAgentUsesGoogleApplicationDefaultCredentials(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "gemini_adc";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = isolated_workspace / "adc_curl_args.txt";
    WriteCurlFixture(bin_dir, args_file);
    WriteGcloudCliFixture(bin_dir, "adc-access-token");

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::GeminiAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    profile_store.set_default(agentos::AuthProviderId::gemini, "adc");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::gemini, agentos::AuthMode::cloud_adc, "adc"),
        .provider = agentos::AuthProviderId::gemini,
        .mode = agentos::AuthMode::cloud_adc,
        .profile_name = "adc",
        .account_label = "google-application-default-credentials",
        .managed_by_agentos = false,
        .managed_by_external_cli = true,
        .refresh_supported = true,
        .headless_compatible = true,
        .access_token_ref = "external-cli:gcloud-adc",
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    Expect(agent.healthy(), "gemini agent should be healthy when ADC can mint a token through gcloud");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "gemini-adc-provider",
        .task_type = "analysis",
        .objective = "Use Google ADC",
        .workspace_path = isolated_workspace.string(),
        .timeout_ms = 5000,
    });

    Expect(result.success, "gemini agent should call Gemini REST with a gcloud ADC token");
    Expect(result.summary == "gemini response", "gemini ADC path should still extract response text");

    const auto args = ReadText(args_file);
    const auto deref = ReadText(std::filesystem::path(args_file.string() + ".deref"));
    Expect(deref.find("Authorization: Bearer adc-access-token") != std::string::npos,
        "gemini ADC path should authenticate REST calls with the minted bearer token (in deref'd headers file)");
    Expect(args.find("adc-access-token") == std::string::npos,
        "gemini ADC path must NOT leak the minted bearer token onto argv");
    Expect(result.structured_output_json.find("adc-access-token") == std::string::npos,
        "gemini ADC structured output should redact the minted bearer token");
}

void TestGeminiAgentUsesExternalGeminiCliOAuthSession(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "gemini_cli_oauth";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    WriteGeminiCliFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::GeminiAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    profile_store.set_default(agentos::AuthProviderId::gemini, "oauth");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::gemini, agentos::AuthMode::browser_oauth, "oauth"),
        .provider = agentos::AuthProviderId::gemini,
        .mode = agentos::AuthMode::browser_oauth,
        .profile_name = "oauth",
        .account_label = "gemini-cli-oauth",
        .managed_by_agentos = false,
        .managed_by_external_cli = true,
        .headless_compatible = false,
        .access_token_ref = "external-cli:gemini",
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    Expect(agent.healthy(), "gemini agent should be healthy with an imported Gemini CLI OAuth session");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "gemini-cli-provider",
        .task_type = "analysis",
        .objective = "Use existing browser OAuth",
        .workspace_path = isolated_workspace.string(),
        .constraints_json = R"({"model":"gemini-3.1-pro"})",
        .timeout_ms = 5000,
    });

    Expect(result.success, "gemini agent should execute through Gemini CLI for external OAuth sessions");
    Expect(result.summary.find("gemini cli response") != std::string::npos,
        "gemini agent should return Gemini CLI output");
    Expect(result.structured_output_json.find("\"auth_source\":\"gemini_cli_oauth\"") != std::string::npos,
        "gemini agent should identify the external CLI OAuth source");
    Expect(result.structured_output_json.find("\"model\":\"gemini-3.1-pro-preview\"") != std::string::npos,
        "gemini agent should normalize the Gemini 3.1 Pro shorthand to the official model id");
    Expect(result.structured_output_json.find("-m gemini-3.1-pro-preview") != std::string::npos,
        "gemini agent should pass the official Gemini 3.1 Pro model id to Gemini CLI");
    Expect(result.structured_output_json.find("exact phrase or exact output") != std::string::npos,
        "gemini agent should pass exact-output guidance into the CLI prompt");

    const auto exact_result = agent.run_task(agentos::AgentTask{
        .task_id = "gemini-cli-exact-provider",
        .task_type = "analysis",
        .objective = "Return exactly: GEMINI_FIXTURE_OK",
        .workspace_path = isolated_workspace.string(),
        .constraints_json = R"({"model":"gemini-3.1-pro"})",
        .timeout_ms = 5000,
    });
    Expect(exact_result.success, "gemini agent should execute exact-output prompts through Gemini CLI");
    Expect(exact_result.structured_output_json.find("Return exactly: GEMINI_FIXTURE_OK") != std::string::npos,
        "gemini agent should pass exact-output objectives as the direct CLI prompt");
    Expect(exact_result.structured_output_json.find("Task id: gemini-cli-exact-provider") == std::string::npos,
        "gemini agent should not wrap exact-output objectives with AgentOS metadata");
}

void WriteClaudeCurlFixture(const std::filesystem::path& bin_dir, const std::filesystem::path& args_file) {
    const auto deref_file = SlashPath(std::filesystem::path(args_file.string() + ".deref"));
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + SlashPath(args_file) + "\"\n"
        "type nul > \"" + deref_file + "\"\n"
        ":next\n"
        "set \"arg=%~1\"\n"
        "if \"%arg%\"==\"\" goto done\n"
        "if \"%arg:~0,1%\"==\"@\" type \"%arg:~1%\" >> \"" + deref_file + "\"\n"
        "shift\n"
        "goto next\n"
        ":done\n"
        "echo {\"content\":[{\"text\":\"claude response\"}]}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
        ": > '" + deref_file + "'\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in\n"
        "    @*) cat \"${arg#@}\" >> '" + deref_file + "' ;;\n"
        "  esac\n"
        "done\n"
        "printf '%s\\n' '{\"content\":[{\"text\":\"claude response\"}]}'\n");
#endif
}

void WriteClaudeCliFixture(const std::filesystem::path& bin_dir) {
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "claude",
        "@echo off\n"
        "echo claude cli response\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "claude",
        "#!/usr/bin/env sh\n"
        "printf 'claude cli response\\n'\n");
#endif
}

void TestAnthropicAgentUsesAuthenticatedProviderSession(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "anthropic_rest";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = isolated_workspace / "curl_args.txt";
    WriteClaudeCurlFixture(bin_dir, args_file);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    SetEnvForTest("AGENTOS_TEST_ANTHROPIC_KEY", "test-anthropic-secret");

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::AnthropicAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    Expect(!agent.healthy(), "anthropic agent should be unhealthy before a session is configured");

    profile_store.set_default(agentos::AuthProviderId::anthropic, "default");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::anthropic, agentos::AuthMode::api_key, "default"),
        .provider = agentos::AuthProviderId::anthropic,
        .mode = agentos::AuthMode::api_key,
        .profile_name = "default",
        .account_label = "anthropic-test",
        .managed_by_agentos = true,
        .headless_compatible = true,
        .access_token_ref = token_store.make_env_ref("AGENTOS_TEST_ANTHROPIC_KEY"),
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    Expect(agent.healthy(), "anthropic agent should be healthy when curl and a session are available");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "claude-provider",
        .task_type = "analysis",
        .objective = "Explain Claude integration",
        .workspace_path = isolated_workspace.string(),
        .timeout_ms = 5000,
    });

    Expect(result.success, "anthropic agent should return a successful result from curl");
    Expect(result.summary == "claude response", "anthropic agent should extract the first text response");
    Expect(result.structured_output_json.find("\"content\":\"claude response\"") != std::string::npos,
        "anthropic agent should expose normalized content");
    Expect(result.structured_output_json.find("\"schema_version\":\"agent_result.v1\"") != std::string::npos,
        "anthropic agent should expose the normalized agent result schema");

    const auto args = ReadText(args_file);
    const auto deref = ReadText(std::filesystem::path(args_file.string() + ".deref"));
    Expect(args.find("api.anthropic.com/v1/messages") != std::string::npos,
        "anthropic agent should call the messages endpoint");
    Expect(deref.find("x-api-key: test-anthropic-secret") != std::string::npos,
        "anthropic agent should authenticate with x-api-key (in deref'd headers file)");
    Expect(deref.find("anthropic-version: 2023-06-01") != std::string::npos,
        "anthropic agent should include the version header (in deref'd headers file)");
    // Phase 1.1 leak guard: API key must not appear on argv.
    Expect(args.find("test-anthropic-secret") == std::string::npos,
        "anthropic agent must NOT pass API key as argv");
    Expect(args.find("@") != std::string::npos,
        "anthropic agent must reference body/header via curl @file syntax");
}

void TestAnthropicAgentUsesExternalClaudeCliSession(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "anthropic_cli";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    WriteClaudeCliFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::AnthropicAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    profile_store.set_default(agentos::AuthProviderId::anthropic, "cli");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::anthropic, agentos::AuthMode::cli_session_passthrough, "cli"),
        .provider = agentos::AuthProviderId::anthropic,
        .mode = agentos::AuthMode::cli_session_passthrough,
        .profile_name = "cli",
        .account_label = "claude-cli-test",
        .managed_by_agentos = false,
        .managed_by_external_cli = true,
        .headless_compatible = false,
        .access_token_ref = "external-cli:claude",
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    Expect(agent.healthy(), "anthropic agent should be healthy with an external CLI session");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "claude-cli-provider",
        .task_type = "analysis",
        .objective = "Use claude CLI",
        .workspace_path = isolated_workspace.string(),
        .timeout_ms = 5000,
    });

    Expect(result.success, "anthropic agent should execute through Claude CLI");
    Expect(result.summary.find("claude cli response") != std::string::npos,
        "anthropic agent should return Claude CLI output");
}

void WriteQwenCurlFixture(const std::filesystem::path& bin_dir, const std::filesystem::path& args_file) {
    const auto deref_file = SlashPath(std::filesystem::path(args_file.string() + ".deref"));
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + SlashPath(args_file) + "\"\n"
        "type nul > \"" + deref_file + "\"\n"
        ":next\n"
        "set \"arg=%~1\"\n"
        "if \"%arg%\"==\"\" goto done\n"
        "if \"%arg:~0,1%\"==\"@\" type \"%arg:~1%\" >> \"" + deref_file + "\"\n"
        "shift\n"
        "goto next\n"
        ":done\n"
        "echo {\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"qwen response\"}}]}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
        ": > '" + deref_file + "'\n"
        "for arg in \"$@\"; do\n"
        "  case \"$arg\" in\n"
        "    @*) cat \"${arg#@}\" >> '" + deref_file + "' ;;\n"
        "  esac\n"
        "done\n"
        "printf '%s\\n' '{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"qwen response\"}}]}'\n");
#endif
}

void TestQwenAgentUsesAuthenticatedProviderSession(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "qwen_rest";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = isolated_workspace / "curl_args.txt";
    WriteQwenCurlFixture(bin_dir, args_file);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    SetEnvForTest("AGENTOS_TEST_QWEN_KEY", "test-qwen-secret");

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::QwenAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    Expect(!agent.healthy(), "qwen agent should be unhealthy before a session is configured");

    profile_store.set_default(agentos::AuthProviderId::qwen, "team");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::qwen, agentos::AuthMode::api_key, "team"),
        .provider = agentos::AuthProviderId::qwen,
        .mode = agentos::AuthMode::api_key,
        .profile_name = "team",
        .account_label = "qwen-test",
        .managed_by_agentos = true,
        .headless_compatible = true,
        .access_token_ref = token_store.make_env_ref("AGENTOS_TEST_QWEN_KEY"),
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    Expect(agent.healthy(), "qwen agent should be healthy when curl and a qwen session are available");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "qwen-provider",
        .task_type = "analysis",
        .objective = "Explain Qwen integration",
        .workspace_path = isolated_workspace.string(),
        .constraints_json = R"({"model":"qwen-plus"})",
        .timeout_ms = 5000,
    });

    Expect(result.success, "qwen agent should return a successful result from curl");
    Expect(result.summary == "qwen response", "qwen agent should extract the first assistant message");
    Expect(result.structured_output_json.find("\"content\":\"qwen response\"") != std::string::npos,
        "qwen agent should expose normalized content");
    Expect(result.structured_output_json.find("\"schema_version\":\"agent_result.v1\"") != std::string::npos,
        "qwen agent should expose the normalized agent result schema");
    Expect(result.structured_output_json.find("\"profile\":\"team\"") != std::string::npos,
        "qwen agent should use the configured default profile");
    Expect(result.structured_output_json.find("\"model\":\"qwen-plus\"") != std::string::npos,
        "qwen agent should preserve the requested qwen model");
    Expect(result.structured_output_json.find("test-qwen-secret") == std::string::npos,
        "qwen agent structured output should redact the API key");

    const auto args = ReadText(args_file);
    const auto deref = ReadText(std::filesystem::path(args_file.string() + ".deref"));
    Expect(args.find("dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions") != std::string::npos,
        "qwen agent should call the Model Studio OpenAI-compatible chat completions endpoint");
    Expect(deref.find("Authorization: Bearer test-qwen-secret") != std::string::npos,
        "qwen agent should authenticate Qwen API-key sessions with bearer auth (in deref'd headers file)");
    Expect(deref.find("Explain Qwen integration") != std::string::npos,
        "qwen agent should send the task objective in the request body (deref'd)");
    // Phase 1.1 leak guard: bearer token must not appear on argv.
    Expect(args.find("test-qwen-secret") == std::string::npos,
        "qwen agent must NOT pass API key as argv");
    Expect(args.find("Bearer test-qwen-secret") == std::string::npos,
        "qwen agent must NOT pass Bearer token as argv");
    Expect(args.find("@") != std::string::npos,
        "qwen agent must reference body/header via curl @file syntax");
}

// Phase 1.1 dedicated leak guard: prove that the Gemini agent's REST path
// never spills the bearer token onto curl's argv (which would be visible
// via /proc/<pid>/cmdline on POSIX or the process listing on Windows).
// Mirrors `TestGeminiAgentUsesAuthenticatedProviderSession` but uses an
// OAuth/bearer session and asserts strictly on the captured argv.
void TestGeminiAgentDoesNotLeakBearerTokenOnArgv(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "gemini_argv_leak_guard";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = isolated_workspace / "leak_guard_curl_args.txt";
    WriteCurlFixture(bin_dir, args_file);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    constexpr const char* kSecretToken = "phase11-bearer-token-must-not-leak-to-argv";
    SetEnvForTest("AGENTOS_TEST_GEMINI_OAUTH_TOKEN", kSecretToken);

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::GeminiAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    // Use an OAuth-style bearer session so the agent emits
    // `Authorization: Bearer <token>` rather than `x-goog-api-key`.
    profile_store.set_default(agentos::AuthProviderId::gemini, "oauth-bearer");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(
            agentos::AuthProviderId::gemini,
            agentos::AuthMode::api_key,
            "oauth-bearer"),
        .provider = agentos::AuthProviderId::gemini,
        .mode = agentos::AuthMode::api_key,  // api_key mode triggers x-goog-api-key path; we still test argv-leak there
        .profile_name = "oauth-bearer",
        .account_label = "leak-guard",
        .managed_by_agentos = true,
        .headless_compatible = true,
        .access_token_ref = token_store.make_env_ref("AGENTOS_TEST_GEMINI_OAUTH_TOKEN"),
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "gemini-leak-guard",
        .task_type = "analysis",
        .objective = "Trigger gemini REST path",
        .workspace_path = isolated_workspace.string(),
        .timeout_ms = 5000,
    });

    Expect(result.success, "gemini agent leak-guard task should succeed against the fixture");

    const auto args = ReadText(args_file);
    const auto deref = ReadText(std::filesystem::path(args_file.string() + ".deref"));

    // The whole point: the secret token must NOT appear on the curl process argv.
    Expect(args.find(kSecretToken) == std::string::npos,
        "Phase 1.1: gemini agent must NOT spill bearer/api-key token onto curl argv");
    Expect(args.find("Bearer phase11-bearer-token-must-not-leak-to-argv") == std::string::npos,
        "Phase 1.1: gemini agent must NOT pass `Bearer <token>` as a -H argv");
    Expect(args.find("x-goog-api-key: phase11-bearer-token-must-not-leak-to-argv") == std::string::npos,
        "Phase 1.1: gemini agent must NOT pass `x-goog-api-key: <token>` as a -H argv");

    // It MUST use curl's @file deref syntax instead.
    Expect(args.find("@") != std::string::npos,
        "Phase 1.1: gemini agent must reference body/header file via curl @file syntax");

    // And the secret SHOULD still reach the request — just via the file.
    Expect(deref.find(kSecretToken) != std::string::npos,
        "Phase 1.1: gemini agent should still send the token, but only via the deref'd headers file");
}

// ----- Phase 4.2: Codex CLI V2 streaming/cancellation fixtures + tests -------
//
// We can't run the real `codex` binary in CI. We install a fixture that emits
// synthetic NDJSON on stdout matching the shapes the V2 adapter parses
// (system, assistant.message.delta, usage, task_complete, error).

void WriteCodexStreamingNdjsonFixture(const std::filesystem::path& bin_dir) {
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "codex",
        "@echo off\n"
        "echo {\"type\":\"system\",\"msg\":{\"session_id\":\"sess-fixture-1\",\"model\":\"gpt-test\"}}\n"
        "echo {\"type\":\"assistant.message.delta\",\"msg\":{\"delta\":\"hello world\"}}\n"
        "echo {\"type\":\"usage\",\"msg\":{\"input_tokens\":42,\"output_tokens\":7,\"cost_usd\":0.0001}}\n"
        "echo {\"type\":\"task_complete\",\"msg\":{\"last_agent_message\":\"hello world\"}}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "codex",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' '{\"type\":\"system\",\"msg\":{\"session_id\":\"sess-fixture-1\",\"model\":\"gpt-test\"}}'\n"
        "printf '%s\\n' '{\"type\":\"assistant.message.delta\",\"msg\":{\"delta\":\"hello world\"}}'\n"
        "printf '%s\\n' '{\"type\":\"usage\",\"msg\":{\"input_tokens\":42,\"output_tokens\":7,\"cost_usd\":0.0001}}'\n"
        "printf '%s\\n' '{\"type\":\"task_complete\",\"msg\":{\"last_agent_message\":\"hello world\"}}'\n");
#endif
}

void WriteCodexSlowFixture(const std::filesystem::path& bin_dir) {
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "codex",
        "@echo off\n"
        "echo {\"type\":\"system\",\"msg\":{\"session_id\":\"sess-slow\",\"model\":\"gpt-slow\"}}\n"
        "powershell -NoProfile -Command \"Start-Sleep -Milliseconds 1000\" >nul\n"
        "echo {\"type\":\"task_complete\",\"msg\":{\"last_agent_message\":\"too late\"}}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "codex",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' '{\"type\":\"system\",\"msg\":{\"session_id\":\"sess-slow\",\"model\":\"gpt-slow\"}}'\n"
        "sleep 1\n"
        "printf '%s\\n' '{\"type\":\"task_complete\",\"msg\":{\"last_agent_message\":\"too late\"}}'\n");
#endif
}

void TestCodexCliAgentLegacyRunTaskStillWorks(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "codex_legacy";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    // Use the streaming fixture; legacy path captures stdout verbatim.
    WriteCodexStreamingNdjsonFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::CodexCliAgent agent(cli_host, isolated_workspace);

    Expect(agent.healthy(), "codex agent should be healthy when fixture binary is on PATH");

    const auto result = agent.run_task(agentos::AgentTask{
        .task_id = "codex-legacy",
        .task_type = "analysis",
        .objective = "smoke test legacy path",
        .workspace_path = isolated_workspace.string(),
        .timeout_ms = 5000,
    });

    Expect(result.success, "codex legacy run_task should succeed against the fixture");
}

void TestCodexCliAgentV2StreamingPath(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "codex_v2_stream";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    WriteCodexStreamingNdjsonFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::CodexCliAgent agent(cli_host, isolated_workspace);
    Expect(agent.healthy(), "codex agent should be healthy when fixture binary is on PATH");

    std::vector<agentos::AgentEvent::Kind> seen;
    auto on_event = [&](const agentos::AgentEvent& ev) -> bool {
        seen.push_back(ev.kind);
        return true;
    };

    agentos::AgentInvocation inv;
    inv.task_id = "codex-v2-stream";
    inv.objective = "stream";
    inv.workspace_path = isolated_workspace;
    inv.timeout_ms = 5000;
    inv.cancel = std::make_shared<agentos::CancellationToken>();

    const auto result = agent.invoke(inv, on_event);

    Expect(result.success, "codex V2 invoke should succeed against the fixture");
    Expect(result.usage.input_tokens > 0,
        "codex V2 should populate AgentUsage.input_tokens from the usage event");
    Expect(result.usage.output_tokens == 7,
        "codex V2 should populate AgentUsage.output_tokens from the usage event");
    Expect(result.session_id.has_value() && *result.session_id == "sess-fixture-1",
        "codex V2 should report the upstream session_id from the system event");
    Expect(seen.size() == 4,
        "codex V2 streaming fixture should emit exactly 4 AgentEvents");
    if (seen.size() == 4) {
        Expect(seen[0] == agentos::AgentEvent::Kind::SessionInit,
            "codex V2 first event should be SessionInit");
        Expect(seen[1] == agentos::AgentEvent::Kind::TextDelta,
            "codex V2 second event should be TextDelta");
        Expect(seen[2] == agentos::AgentEvent::Kind::Usage,
            "codex V2 third event should be Usage");
        Expect(seen[3] == agentos::AgentEvent::Kind::Final,
            "codex V2 fourth event should be Final");
    }
    Expect(result.summary.find("hello world") != std::string::npos,
        "codex V2 summary should reflect final message text");
}

void TestCodexCliAgentV2SyncFallbackPath(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "codex_v2_sync";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    WriteCodexStreamingNdjsonFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::CodexCliAgent agent(cli_host, isolated_workspace);

    agentos::AgentInvocation inv;
    inv.task_id = "codex-v2-sync";
    inv.objective = "sync";
    inv.workspace_path = isolated_workspace;
    inv.timeout_ms = 5000;
    inv.cancel = std::make_shared<agentos::CancellationToken>();

    // No on_event callback — adapter should still parse the stream and return
    // a populated AgentResult.
    const auto result = agent.invoke(inv);

    Expect(result.success, "codex V2 sync (no callback) should still succeed");
    Expect(result.usage.input_tokens == 42,
        "codex V2 sync path should still populate AgentUsage.input_tokens");
    Expect(result.usage.output_tokens == 7,
        "codex V2 sync path should still populate AgentUsage.output_tokens");
    Expect(result.session_id.has_value() && *result.session_id == "sess-fixture-1",
        "codex V2 sync path should still report the upstream session_id");
    Expect(result.summary.find("hello world") != std::string::npos,
        "codex V2 sync path summary should reflect the final message text");
}

void TestCodexCliAgentV2Cancellation(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "codex_v2_cancel";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    WriteCodexSlowFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::CodexCliAgent agent(cli_host, isolated_workspace);

    auto cancel = std::make_shared<agentos::CancellationToken>();

    agentos::AgentInvocation inv;
    inv.task_id = "codex-v2-cancel";
    inv.objective = "cancel-me";
    inv.workspace_path = isolated_workspace;
    inv.timeout_ms = 5000;
    inv.cancel = cancel;

    std::thread killer([cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel->cancel();
    });

    const auto started = std::chrono::steady_clock::now();
    const auto result = agent.invoke(inv);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    if (killer.joinable()) killer.join();

    Expect(elapsed_ms < 2000,
        "codex V2 cancellation should bring invoke() back well under the slow fixture's 1s sleep");
    Expect(!result.success, "codex V2 cancellation should produce an unsuccessful result");
    Expect(result.error_code == "Cancelled",
        "codex V2 cancellation should set error_code=Cancelled");
}


void TestGeminiAgentV2InvokeEmitsLifecycleEvents(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "gemini_v2_invoke";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = isolated_workspace / "v2_curl_args.txt";
    WriteCurlFixture(bin_dir, args_file);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    SetEnvForTest("AGENTOS_TEST_GEMINI_KEY", "test-gemini-secret");

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::GeminiAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    profile_store.set_default(agentos::AuthProviderId::gemini, "team");
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::gemini, agentos::AuthMode::api_key, "team"),
        .provider = agentos::AuthProviderId::gemini,
        .mode = agentos::AuthMode::api_key,
        .profile_name = "team",
        .account_label = "gemini-test",
        .managed_by_agentos = true,
        .headless_compatible = true,
        .access_token_ref = token_store.make_env_ref("AGENTOS_TEST_GEMINI_KEY"),
        .expires_at = std::chrono::system_clock::time_point::max(),
    });

    std::vector<agentos::AgentEvent::Kind> seen;
    std::string seen_model;
    auto on_event = [&](const agentos::AgentEvent& ev) -> bool {
        seen.push_back(ev.kind);
        if (ev.kind == agentos::AgentEvent::Kind::SessionInit) {
            const auto it = ev.fields.find("model");
            if (it != ev.fields.end()) {
                seen_model = it->second;
            }
        }
        return true;
    };

    agentos::AgentInvocation inv;
    inv.task_id = "gemini-v2-invoke";
    inv.objective = "Explain provider integration";
    inv.workspace_path = isolated_workspace;
    inv.timeout_ms = 5000;
    inv.constraints = {{"model", "gemini-3.1-pro"}};
    inv.cancel = std::make_shared<agentos::CancellationToken>();

    const auto result = agent.invoke(inv, on_event);

    Expect(result.success, "gemini V2 invoke should succeed against the curl fixture");
    Expect(result.summary == "gemini response",
        "gemini V2 invoke should extract the first text response");
    Expect(result.usage.turns == 1,
        "gemini V2 invoke should record one turn of explicit zero-cost usage");
    Expect(result.usage.cost_usd == 0.0,
        "gemini V2 invoke should set cost_usd=0 since gemini does not surface cost data");
    Expect(seen.size() == 4,
        "gemini V2 invoke should emit SessionInit, Status, Usage, and Final");
    if (seen.size() == 4) {
        Expect(seen[0] == agentos::AgentEvent::Kind::SessionInit,
            "gemini V2 first event should be SessionInit");
        Expect(seen[1] == agentos::AgentEvent::Kind::Status,
            "gemini V2 second event should be Status");
        Expect(seen[2] == agentos::AgentEvent::Kind::Usage,
            "gemini V2 third event should be Usage");
        Expect(seen[3] == agentos::AgentEvent::Kind::Final,
            "gemini V2 fourth event should be Final");
    }
    Expect(seen_model == "gemini-3.1-pro-preview",
        "gemini V2 SessionInit should carry the normalized model name from invocation.constraints");
}

void TestGeminiAgentV2InvokeCancelsBeforeDispatch(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "gemini_v2_cancel";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(isolated_workspace / "auth" / "profiles.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::GeminiAgent agent(cli_host, credential_broker, profile_store, isolated_workspace);

    auto cancel = std::make_shared<agentos::CancellationToken>();
    cancel->cancel();

    agentos::AgentInvocation inv;
    inv.task_id = "gemini-v2-cancel";
    inv.objective = "should never run";
    inv.workspace_path = isolated_workspace;
    inv.cancel = cancel;

    const auto result = agent.invoke(inv);

    Expect(!result.success, "gemini V2 invoke should fail when the cancellation token is pre-tripped");
    Expect(result.error_code == "Cancelled",
        "gemini V2 cancellation should set error_code=Cancelled");
}

// Direct regression test for the V2 -> legacy projection helpers
// (`InvocationToTask` / `TaskFromInvocation`) on every adapter that owns one.
// These helpers are the bridge that lets `invoke()` reuse `run_task()` for
// sync mode and failure fallbacks; if a future change adds a field to
// `AgentInvocation` and forgets to wire it through any of these helpers, the
// V2 sync path silently loses that field.
//
// The previous round shipped per-task `auth_profile=` overrides but missed
// these projections — `agentos run target=qwen profile=team objective=...`
// silently fell back to the default profile because `InvocationToTask`
// dropped `invocation.auth_profile`. Same shape on openai/gemini/anthropic.
// This test fences that off going forward.
//
// Each adapter's projection has the same contract (see
// docs/AGENT_SYSTEM.md §4.7): copy task_id / objective / workspace_path /
// auth_profile / timeout_ms / budget_limit, encode invocation.context as
// `context_json`, encode invocation.constraints as `constraints_json`, and
// derive `task_type` from `context["task_type"]`.
void TestV2ToLegacyProjectionPropagatesAllFields() {
    agentos::AgentInvocation invocation;
    invocation.task_id = "proj-task-id";
    invocation.objective = "propagate every field";
    invocation.workspace_path = std::filesystem::path("/tmp/projection_workspace");
    invocation.auth_profile = "team-profile";
    invocation.context = {
        {"task_type", "analysis"},
        {"parent_task_id", "parent-007"},
        {"role", "planner"},
    };
    invocation.constraints = {
        {"model", "gpt-4o-mini"},
    };
    invocation.timeout_ms = 17500;
    invocation.budget_limit_usd = 2.5;

    struct Case {
        const char* name;
        agentos::AgentTask task;
    };
    const std::vector<Case> cases = {
        {"qwen",      agentos::QwenAgent::InvocationToTask(invocation)},
        {"openai",    agentos::OpenAiAgent::InvocationToTask(invocation)},
        {"gemini",    agentos::GeminiAgent::TaskFromInvocation(invocation)},
        {"anthropic", agentos::AnthropicAgent::TaskFromInvocation(invocation)},
    };

    for (const auto& [name, task] : cases) {
        const std::string label = std::string(" (") + name + ")";

        Expect(task.task_id == "proj-task-id",
            ("projection should copy task_id" + label));
        Expect(task.objective == "propagate every field",
            ("projection should copy objective" + label));
        Expect(task.workspace_path == "/tmp/projection_workspace" || task.workspace_path == "\\tmp\\projection_workspace",
            ("projection should copy workspace_path as a string" + label));
        Expect(task.auth_profile.has_value() && *task.auth_profile == "team-profile",
            ("projection MUST carry invocation.auth_profile so per-task profile= overrides survive sync fallback" + label));
        Expect(task.timeout_ms == 17500,
            ("projection should copy timeout_ms" + label));
        Expect(task.budget_limit > 2.49 && task.budget_limit < 2.51,
            ("projection should copy budget_limit_usd into budget_limit" + label));

        // task_type is derived from context["task_type"] across all adapters
        // (anthropic uses a literal "agent_invoke" instead — assert per name).
        if (std::string(name) == "anthropic") {
            Expect(task.task_type == "agent_invoke",
                ("anthropic projection sets task_type=agent_invoke literal" + label));
        } else {
            Expect(task.task_type == "analysis",
                ("projection should derive task_type from context[\"task_type\"]" + label));
        }

        // Encoded JSON blobs should contain the original keys/values. Substring
        // checks are sufficient — we don't depend on key ordering, just that
        // the field round-tripped at all (this is exactly what was missing on
        // qwen/openai before this round).
        Expect(task.context_json.find("\"parent_task_id\"") != std::string::npos &&
               task.context_json.find("parent-007") != std::string::npos,
            ("projection should encode invocation.context into context_json" + label));
        Expect(task.context_json.find("\"role\"") != std::string::npos &&
               task.context_json.find("planner") != std::string::npos,
            ("projection should preserve all context entries, not just task_type" + label));
        Expect(task.constraints_json.find("\"model\"") != std::string::npos &&
               task.constraints_json.find("gpt-4o-mini") != std::string::npos,
            ("projection should encode invocation.constraints into constraints_json (legacy model_name() reads this)" + label));
    }

    // Empty maps must produce empty strings, not "{}", so the legacy
    // run_task() path's `!context_json.empty()` / `!constraints_json.empty()`
    // gates keep their existing semantics across all four adapters.
    agentos::AgentInvocation empty_inv;
    empty_inv.task_id = "empty";
    empty_inv.objective = "no maps";
    empty_inv.workspace_path = std::filesystem::path("/tmp/projection_workspace");
    const std::vector<Case> empty_cases = {
        {"qwen",      agentos::QwenAgent::InvocationToTask(empty_inv)},
        {"openai",    agentos::OpenAiAgent::InvocationToTask(empty_inv)},
        {"gemini",    agentos::GeminiAgent::TaskFromInvocation(empty_inv)},
        {"anthropic", agentos::AnthropicAgent::TaskFromInvocation(empty_inv)},
    };
    for (const auto& [name, task] : empty_cases) {
        const std::string label = std::string(" (") + name + ")";
        Expect(task.context_json.empty(),
            ("empty invocation.context should yield empty context_json, not \"{}\"" + label));
        Expect(task.constraints_json.empty(),
            ("empty invocation.constraints should yield empty constraints_json, not \"{}\"" + label));
        Expect(!task.auth_profile.has_value(),
            ("absent invocation.auth_profile should leave task.auth_profile unset" + label));
    }
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestGeminiAgentUsesAuthenticatedProviderSession(workspace);
    TestGeminiAgentUsesGoogleApplicationDefaultCredentials(workspace);
    TestGeminiAgentUsesExternalGeminiCliOAuthSession(workspace);
    TestGeminiAgentV2InvokeEmitsLifecycleEvents(workspace);
    TestGeminiAgentV2InvokeCancelsBeforeDispatch(workspace);
    TestAnthropicAgentUsesAuthenticatedProviderSession(workspace);
    TestAnthropicAgentUsesExternalClaudeCliSession(workspace);
    TestQwenAgentUsesAuthenticatedProviderSession(workspace);
    TestGeminiAgentDoesNotLeakBearerTokenOnArgv(workspace);
    TestV2ToLegacyProjectionPropagatesAllFields();

    if (failures != 0) {
        std::cerr << failures << " agent provider test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_agent_provider_tests passed\n";
    return 0;
}
