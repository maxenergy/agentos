#include "auth/auth_models.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/agents/gemini_agent.hpp"
#include "hosts/agents/anthropic_agent.hpp"
#include "hosts/agents/qwen_agent.hpp"
#include "hosts/cli/cli_host.hpp"
#include "test_command_fixtures.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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

void WriteCurlFixture(const std::filesystem::path& bin_dir, const std::filesystem::path& args_file) {
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + SlashPath(args_file) + "\"\n"
        "echo {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gemini response\"}]}}]}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
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
    Expect(args.find("generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent") != std::string::npos,
        "gemini agent should call the generateContent endpoint");
    Expect(args.find("x-goog-api-key: test-gemini-secret") != std::string::npos,
        "gemini agent should authenticate API-key sessions with x-goog-api-key");
    Expect(args.find("Explain provider integration") != std::string::npos,
        "gemini agent should send the task objective in the request body");
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
    Expect(args.find("Authorization: Bearer adc-access-token") != std::string::npos,
        "gemini ADC path should authenticate REST calls with the minted bearer token");
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
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + SlashPath(args_file) + "\"\n"
        "echo {\"content\":[{\"text\":\"claude response\"}]}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
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
    Expect(args.find("api.anthropic.com/v1/messages") != std::string::npos,
        "anthropic agent should call the messages endpoint");
    Expect(args.find("x-api-key: test-anthropic-secret") != std::string::npos,
        "anthropic agent should authenticate with x-api-key");
    Expect(args.find("anthropic-version: 2023-06-01") != std::string::npos,
        "anthropic agent should include the version header");
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
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + SlashPath(args_file) + "\"\n"
        "echo {\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"qwen response\"}}]}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
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
    Expect(args.find("dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions") != std::string::npos,
        "qwen agent should call the Model Studio OpenAI-compatible chat completions endpoint");
    Expect(args.find("Authorization: Bearer test-qwen-secret") != std::string::npos,
        "qwen agent should authenticate Qwen API-key sessions with bearer auth");
    Expect(args.find("Explain Qwen integration") != std::string::npos,
        "qwen agent should send the task objective in the request body");
}


}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestGeminiAgentUsesAuthenticatedProviderSession(workspace);
    TestGeminiAgentUsesGoogleApplicationDefaultCredentials(workspace);
    TestGeminiAgentUsesExternalGeminiCliOAuthSession(workspace);
    TestAnthropicAgentUsesAuthenticatedProviderSession(workspace);
    TestAnthropicAgentUsesExternalClaudeCliSession(workspace);
    TestQwenAgentUsesAuthenticatedProviderSession(workspace);

    if (failures != 0) {
        std::cerr << failures << " agent provider test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_agent_provider_tests passed\n";
    return 0;
}
