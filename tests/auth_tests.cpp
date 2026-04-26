#include "auth/auth_manager.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "auth/oauth_pkce.hpp"
#include "auth/provider_adapters.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/cli/cli_host.hpp"
#include "test_command_fixtures.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using agentos::test::ClearEnvForTest;
using agentos::test::PrependPathForTest;
using agentos::test::ReadEnvForTest;
using agentos::test::ScopedEnvOverride;
using agentos::test::SetEnvForTest;
using agentos::test::WriteClaudeCliFixture;
using agentos::test::WriteCodexCliFixture;
using agentos::test::WriteCliFixture;
using agentos::test::WriteGeminiCliFixture;
using agentos::test::WriteGcloudCliFixture;

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_auth_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

void WriteOAuthCurlFixture(const std::filesystem::path& bin_dir, const std::filesystem::path& args_file) {
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + args_file.generic_string() + "\"\n"
        "echo {\"access_token\":\"curl-access\",\"refresh_token\":\"curl-refresh\",\"token_type\":\"Bearer\",\"expires_in\":1800}\n"
        "exit /b 0\n");
#else
    WriteCliFixture(
        bin_dir,
        "curl",
        "#!/usr/bin/env sh\n"
        "printf '%s\\n' \"$*\" > '" + args_file.string() + "'\n"
        "printf '%s\\n' '{\"access_token\":\"curl-access\",\"refresh_token\":\"curl-refresh\",\"token_type\":\"Bearer\",\"expires_in\":1800}'\n");
#endif
}

#ifdef _WIN32
using TestSocketHandle = SOCKET;
constexpr TestSocketHandle kInvalidTestSocket = INVALID_SOCKET;
void CloseTestSocket(const TestSocketHandle socket_handle) {
    closesocket(socket_handle);
}
class TestWinsockSession {
public:
    TestWinsockSession() {
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~TestWinsockSession() {
        if (ok_) {
            WSACleanup();
        }
    }
private:
    bool ok_ = false;
};
#else
using TestSocketHandle = int;
constexpr TestSocketHandle kInvalidTestSocket = -1;
void CloseTestSocket(const TestSocketHandle socket_handle) {
    close(socket_handle);
}
class TestWinsockSession {
};
#endif

int FindFreeLoopbackPortForTest() {
    TestWinsockSession winsock;
    const TestSocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == kInvalidTestSocket) {
        return 48179;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CloseTestSocket(socket_handle);
        return 48179;
    }
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (getsockname(socket_handle, reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        CloseTestSocket(socket_handle);
        return 48179;
    }
    const auto port = ntohs(bound.sin_port);
    CloseTestSocket(socket_handle);
    return static_cast<int>(port);
}

std::string SendHttpGetForTest(const int port, const std::string& target) {
    TestWinsockSession winsock;
    const TestSocketHandle socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == kInvalidTestSocket) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CloseTestSocket(socket_handle);
        return {};
    }
    const auto request =
        "GET " + target + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    (void)send(socket_handle, request.c_str(), static_cast<int>(request.size()), 0);
    std::string response(1024, '\0');
    const auto received = recv(socket_handle, response.data(), static_cast<int>(response.size()), 0);
    CloseTestSocket(socket_handle);
    if (received <= 0) {
        return {};
    }
    response.resize(static_cast<std::size_t>(received));
    return response;
}

void TestAuthApiKeySession(const std::filesystem::path& workspace) {
    SetEnvForTest("AGENTOS_TEST_QWEN_KEY", "test-secret");

    agentos::SessionStore session_store(workspace / "auth" / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::CredentialBroker credential_broker(session_store, token_store);
    agentos::AuthManager auth_manager(session_store);

    auth_manager.register_provider(std::make_shared<agentos::QwenAuthProviderAdapter>(session_store, token_store));

    const auto token_store_status = token_store.status();
#ifdef _WIN32
    Expect(token_store_status.backend_name == "windows-credential-manager",
        "secure token store should report Windows Credential Manager backend on Windows");
    Expect(token_store_status.system_keychain_backed, "secure token store should claim system credential support on Windows");
    Expect(!token_store_status.dev_only, "Windows credential store should not be marked dev-only");

    const auto managed_ref = token_store.write_managed_token("qwen", "smoke", "access", "managed-secret");
    Expect(managed_ref.rfind("wincred:", 0) == 0, "managed token refs should use wincred prefix on Windows");
    Expect(token_store.read_ref(managed_ref) == "managed-secret", "secure token store should read managed Windows credentials");
    Expect(token_store.ref_available(managed_ref), "secure token store should report managed Windows credentials as available");
    Expect(token_store.delete_ref(managed_ref), "secure token store should delete managed Windows credentials");
    Expect(!token_store.ref_available(managed_ref), "deleted managed Windows credentials should no longer be available");
#else
    Expect(token_store_status.backend_name == "env-ref-only", "secure token store should report env-ref-only MVP backend");
    Expect(!token_store_status.system_keychain_backed, "secure token store should not claim system keychain support");
    Expect(token_store_status.dev_only, "secure token store fallback should be marked dev-only");
#endif

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

    bool refresh_failed = false;
    try {
        (void)auth_manager.refresh(agentos::AuthProviderId::qwen, "smoke");
    } catch (const std::exception&) {
        refresh_failed = true;
    }
    Expect(refresh_failed, "api-key session refresh should be unsupported");
}

void TestOAuthPkceScaffold(const std::filesystem::path& workspace) {
    const auto challenge = agentos::CreatePkceCodeChallengeForTest(
        "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
    Expect(challenge == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
        "PKCE S256 challenge should match the RFC 7636 example");

    const auto start = agentos::CreateOAuthPkceStart(agentos::OAuthPkceStartRequest{
        .provider = agentos::AuthProviderId::gemini,
        .profile_name = "pkce",
        .authorization_endpoint = "https://accounts.example.test/o/oauth2/v2/auth",
        .client_id = "client-id",
        .redirect_uri = "http://127.0.0.1:48177/callback",
        .scopes = {"openid", "email"},
    });
    Expect(start.provider == agentos::AuthProviderId::gemini, "PKCE start should preserve provider");
    Expect(start.profile_name == "pkce", "PKCE start should preserve profile name");
    Expect(start.code_challenge_method == "S256", "PKCE start should use S256 challenge method");
    Expect(start.state.size() >= 32, "PKCE state should contain random entropy");
    Expect(start.code_verifier.size() >= 43, "PKCE verifier should satisfy minimum RFC length");
    Expect(start.code_challenge == agentos::CreatePkceCodeChallengeForTest(start.code_verifier),
        "PKCE start should derive challenge from verifier");
    Expect(start.authorization_url.find("response_type=code") != std::string::npos,
        "PKCE authorization URL should request authorization code flow");
    Expect(start.authorization_url.find("code_challenge_method=S256") != std::string::npos,
        "PKCE authorization URL should include S256 method");
    Expect(start.authorization_url.find("scope=openid%20email") != std::string::npos,
        "PKCE authorization URL should encode scopes");

    const auto deterministic_start = agentos::CreateOAuthPkceStart(agentos::OAuthPkceStartRequest{
        .provider = agentos::AuthProviderId::gemini,
        .profile_name = "pkce-fixed",
        .authorization_endpoint = "https://accounts.example.test/o/oauth2/v2/auth",
        .client_id = "client-id",
        .redirect_uri = "http://127.0.0.1:48177/callback",
        .state = "fixed-state",
        .code_verifier = "fixed-verifier",
    });
    Expect(deterministic_start.state == "fixed-state", "PKCE start should accept caller-provided state");
    Expect(deterministic_start.code_verifier == "fixed-verifier",
        "PKCE start should accept caller-provided code verifier");
    Expect(deterministic_start.authorization_url.find("state=fixed-state") != std::string::npos,
        "PKCE authorization URL should include caller-provided state");

    const auto gemini_defaults = agentos::OAuthDefaultsForProvider(agentos::AuthProviderId::gemini);
    Expect(gemini_defaults.supported, "Gemini should have OAuth provider defaults");
    Expect(gemini_defaults.authorization_endpoint == "https://accounts.google.com/o/oauth2/v2/auth",
        "Gemini OAuth defaults should include authorization endpoint");
    Expect(gemini_defaults.token_endpoint == "https://oauth2.googleapis.com/token",
        "Gemini OAuth defaults should include token endpoint");
    const auto merged_defaults = agentos::MergeOAuthProviderDefaults(
        gemini_defaults,
        agentos::OAuthProviderDefaults{
            .authorization_endpoint = "https://accounts.example.test/oauth",
            .scopes = {"openid", "email"},
        });
    Expect(merged_defaults.supported, "merged OAuth defaults should remain supported when token endpoint comes from base defaults");
    Expect(merged_defaults.authorization_endpoint == "https://accounts.example.test/oauth",
        "configured OAuth defaults should override non-empty endpoint fields");
    Expect(merged_defaults.token_endpoint == "https://oauth2.googleapis.com/token",
        "configured OAuth defaults should preserve base token endpoint when omitted");
    Expect(merged_defaults.scopes.size() == 2 && merged_defaults.scopes[1] == "email",
        "configured OAuth defaults should override scopes when provided");
    const auto default_start = agentos::CreateOAuthPkceStart(agentos::OAuthPkceStartRequest{
        .provider = agentos::AuthProviderId::gemini,
        .profile_name = "pkce-defaults",
        .client_id = "client-id",
        .redirect_uri = "http://127.0.0.1:48177/callback",
    });
    Expect(default_start.authorization_url.find("https://accounts.google.com/o/oauth2/v2/auth") != std::string::npos,
        "PKCE start should use provider default authorization endpoint");
    Expect(default_start.authorization_url.find("cloud-platform") != std::string::npos,
        "PKCE start should include provider default scopes");

    const auto valid_callback = agentos::ValidateOAuthCallback(start, start.state, "auth-code");
    Expect(valid_callback.success, "PKCE callback should accept matching state and code");
    Expect(valid_callback.code == "auth-code", "PKCE callback should return authorization code");

    const auto valid_callback_url = agentos::ValidateOAuthCallbackUrl(
        start,
        "http://127.0.0.1:48177/callback?code=auth%2Fcode&state=" + start.state + "#ignored");
    Expect(valid_callback_url.success, "PKCE callback URL parser should accept matching state and code");
    Expect(valid_callback_url.code == "auth/code", "PKCE callback URL parser should URL-decode code");

    const auto invalid_state = agentos::ValidateOAuthCallback(start, "wrong-state", "auth-code");
    Expect(!invalid_state.success, "PKCE callback should reject mismatched state");
    Expect(invalid_state.error == "InvalidOAuthState", "PKCE callback should report stable state error");

    const auto invalid_state_url = agentos::ValidateOAuthCallbackUrl(
        start,
        "http://127.0.0.1:48177/callback?code=auth-code&state=wrong-state");
    Expect(!invalid_state_url.success, "PKCE callback URL parser should reject mismatched state");
    Expect(invalid_state_url.error == "InvalidOAuthState",
        "PKCE callback URL parser should preserve stable state error");

    const auto provider_error = agentos::ValidateOAuthCallback(start, start.state, "", "access_denied", "denied");
    Expect(!provider_error.success, "PKCE callback should preserve provider errors");
    Expect(provider_error.error == "access_denied", "PKCE callback should return provider error code");

    const auto provider_error_url = agentos::ValidateOAuthCallbackUrl(
        start,
        "http://127.0.0.1:48177/callback?error=access_denied&error_description=user+denied&state=" + start.state);
    Expect(!provider_error_url.success, "PKCE callback URL parser should preserve provider errors");
    Expect(provider_error_url.error_description == "user denied",
        "PKCE callback URL parser should decode provider error descriptions");

    const auto callback_port = FindFreeLoopbackPortForTest();
    auto listener = std::async(std::launch::async, [&]() {
        return agentos::ListenForOAuthCallbackOnce(agentos::OAuthCallbackListenRequest{
            .start = start,
            .port = callback_port,
            .timeout_ms = 5000,
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto callback_response = SendHttpGetForTest(
        callback_port,
        "/callback?code=listener-code&state=" + start.state);
    const auto listener_result = listener.get();
    Expect(listener_result.success, "OAuth callback listener should accept a loopback GET callback");
    Expect(listener_result.code == "listener-code", "OAuth callback listener should return callback code");
    Expect(callback_response.find("200 OK") != std::string::npos,
        "OAuth callback listener should return a successful HTTP response");

    const auto invalid_callback_port = FindFreeLoopbackPortForTest();
    auto invalid_listener = std::async(std::launch::async, [&]() {
        return agentos::ListenForOAuthCallbackOnce(agentos::OAuthCallbackListenRequest{
            .start = start,
            .port = invalid_callback_port,
            .timeout_ms = 5000,
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto invalid_callback_response = SendHttpGetForTest(
        invalid_callback_port,
        "/callback?code=listener-code&state=wrong-state");
    const auto invalid_listener_result = invalid_listener.get();
    Expect(!invalid_listener_result.success, "OAuth callback listener should reject invalid state");
    Expect(invalid_listener_result.error == "InvalidOAuthState",
        "OAuth callback listener should preserve state validation errors");
    Expect(invalid_callback_response.find("400 Bad Request") != std::string::npos,
        "OAuth callback listener should return a failed HTTP response for invalid state");

    const auto token_request = agentos::BuildOAuthTokenRequest(agentos::OAuthTokenRequestInput{
        .token_endpoint = "https://oauth2.example.test/token",
        .client_id = "client id",
        .redirect_uri = "http://127.0.0.1:48177/callback",
        .code = "code/value",
        .code_verifier = "verifier~value",
    });
    Expect(token_request.token_endpoint == "https://oauth2.example.test/token",
        "OAuth token request should preserve endpoint");
    Expect(token_request.content_type == "application/x-www-form-urlencoded",
        "OAuth token request should use form content type");
    Expect(token_request.body.find("grant_type=authorization_code") != std::string::npos,
        "OAuth token request should use authorization_code grant");
    Expect(token_request.body.find("client_id=client%20id") != std::string::npos,
        "OAuth token request should URL-encode client id");
    Expect(token_request.body.find("code=code%2Fvalue") != std::string::npos,
        "OAuth token request should URL-encode authorization code");
    Expect(token_request.body.find("code_verifier=verifier~value") != std::string::npos,
        "OAuth token request should include PKCE verifier");

    const auto refresh_request = agentos::BuildOAuthRefreshTokenRequest(agentos::OAuthRefreshTokenRequestInput{
        .token_endpoint = "https://oauth2.example.test/token",
        .client_id = "client id",
        .refresh_token = "refresh/value",
    });
    Expect(refresh_request.body.find("grant_type=refresh_token") != std::string::npos,
        "OAuth refresh request should use refresh_token grant");
    Expect(refresh_request.body.find("refresh_token=refresh%2Fvalue") != std::string::npos,
        "OAuth refresh request should URL-encode refresh token");
    Expect(refresh_request.body.find("client_id=client%20id") != std::string::npos,
        "OAuth refresh request should URL-encode client id");

    const auto token_response = agentos::ParseOAuthTokenResponse(
        R"({"access_token":"access","refresh_token":"refresh","token_type":"Bearer","expires_in":3600})");
    Expect(token_response.success, "OAuth token response parser should accept valid token responses");
    Expect(token_response.access_token == "access", "OAuth token response parser should extract access_token");
    Expect(token_response.refresh_token == "refresh", "OAuth token response parser should extract refresh_token");
    Expect(token_response.expires_in_seconds == 3600, "OAuth token response parser should extract expires_in");

    const auto token_error = agentos::ParseOAuthTokenResponse(
        R"({"error":"invalid_grant","error_description":"expired code"})");
    Expect(!token_error.success, "OAuth token response parser should preserve token endpoint errors");
    Expect(token_error.error == "invalid_grant", "OAuth token response parser should extract error");

    const auto missing_token = agentos::ParseOAuthTokenResponse(R"({"token_type":"Bearer"})");
    Expect(!missing_token.success, "OAuth token response parser should reject missing access_token");
    Expect(missing_token.error == "MissingAccessToken", "OAuth token response parser should report missing access token");

    const auto bin_dir = workspace / "oauth_pkce" / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = workspace / "oauth_pkce" / "curl_args.txt";
    WriteOAuthCurlFixture(bin_dir, args_file);
    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    const auto exchanged = agentos::ExecuteOAuthTokenExchange(
        cli_host,
        agentos::OAuthTokenRequestInput{
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .redirect_uri = "http://127.0.0.1:48177/callback",
            .code = "auth-code",
            .code_verifier = "verifier",
        },
        workspace);
    Expect(exchanged.success, "OAuth token exchange should execute through the controlled CLI host");
    Expect(exchanged.access_token == "curl-access", "OAuth token exchange should parse access token from curl response");
    Expect(exchanged.refresh_token == "curl-refresh", "OAuth token exchange should parse refresh token from curl response");
    Expect(exchanged.expires_in_seconds == 1800, "OAuth token exchange should parse token expiry from curl response");
    std::ifstream args_input(args_file, std::ios::binary);
    const std::string curl_args((std::istreambuf_iterator<char>(args_input)), std::istreambuf_iterator<char>());
    Expect(curl_args.find("-X POST") != std::string::npos, "OAuth token exchange should use POST");
    Expect(curl_args.find("Content-Type: application/x-www-form-urlencoded") != std::string::npos,
        "OAuth token exchange should send form content type");
    Expect(curl_args.find("grant_type=authorization_code") != std::string::npos,
        "OAuth token exchange should send authorization_code grant body");

    const auto refreshed = agentos::ExecuteOAuthRefreshTokenExchange(
        cli_host,
        agentos::OAuthRefreshTokenRequestInput{
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .refresh_token = "refresh-token",
        },
        workspace);
    Expect(refreshed.success, "OAuth refresh token exchange should execute through the controlled CLI host");
    Expect(refreshed.access_token == "curl-access", "OAuth refresh token exchange should parse refreshed access token");
    std::ifstream refresh_args_input(args_file, std::ios::binary);
    const std::string refresh_curl_args(
        (std::istreambuf_iterator<char>(refresh_args_input)), std::istreambuf_iterator<char>());
    Expect(refresh_curl_args.find("grant_type=refresh_token") != std::string::npos,
        "OAuth refresh token exchange should send refresh_token grant body");

#ifdef _WIN32
    agentos::SessionStore oauth_session_store(workspace / "oauth_pkce" / "sessions.tsv");
    agentos::SecureTokenStore oauth_token_store;
    const auto orchestrated = agentos::CompleteOAuthLogin(
        cli_host,
        oauth_session_store,
        oauth_token_store,
        agentos::OAuthLoginOrchestrationInput{
            .start = start,
            .callback = valid_callback,
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .account_label = "orchestrated@example.test",
        },
        workspace);
    Expect(orchestrated.provider == agentos::AuthProviderId::gemini,
        "OAuth login orchestration should preserve provider");
    Expect(orchestrated.profile_name == "pkce",
        "OAuth login orchestration should preserve profile");
    Expect(oauth_token_store.read_ref(orchestrated.access_token_ref) == "curl-access",
        "OAuth login orchestration should store exchanged access token");
    Expect(oauth_token_store.read_ref(orchestrated.refresh_token_ref) == "curl-refresh",
        "OAuth login orchestration should store exchanged refresh token");

    const auto orchestrated_refresh = agentos::RefreshOAuthSession(
        cli_host,
        oauth_session_store,
        oauth_token_store,
        agentos::OAuthRefreshOrchestrationInput{
            .existing_session = orchestrated,
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
        },
        workspace);
    Expect(orchestrated_refresh.metadata.contains("refreshed_by"),
        "OAuth refresh orchestration should record refresh metadata");
    Expect(oauth_token_store.read_ref(orchestrated_refresh.access_token_ref) == "curl-access",
        "OAuth refresh orchestration should store refreshed access token");

    agentos::SessionStore native_session_store(workspace / "oauth_pkce" / "native_sessions.tsv");
    agentos::SecureTokenStore native_token_store;
    agentos::AuthManager native_auth_manager(native_session_store);
    native_auth_manager.register_provider(
        std::make_shared<agentos::GeminiAuthProviderAdapter>(
            native_session_store,
            native_token_store,
            cli_host,
            workspace));
    const auto native_login = native_auth_manager.login(
        agentos::AuthProviderId::gemini,
        agentos::AuthMode::browser_oauth,
        {
            {"profile", "native"},
            {"callback_url", "http://127.0.0.1:48177/callback?code=native-code&state=native-state"},
            {"state", "native-state"},
            {"code_verifier", "native-verifier"},
            {"redirect_uri", "http://127.0.0.1:48177/callback"},
            {"client_id", "native-client"},
            {"account_label", "native@example.test"},
        });
    Expect(native_login.managed_by_agentos, "native browser OAuth login should persist an AgentOS-managed session");
    Expect(native_login.metadata.at("credential_source") == "oauth_pkce",
        "native browser OAuth login should record OAuth credential source");
    Expect(native_login.metadata.at("token_endpoint") == "https://oauth2.googleapis.com/token",
        "native browser OAuth login should persist provider default token endpoint for refresh");
    Expect(native_token_store.read_ref(native_login.access_token_ref) == "curl-access",
        "native browser OAuth login should store access token");
    const auto native_refreshed = native_auth_manager.refresh(agentos::AuthProviderId::gemini, "native");
    Expect(native_refreshed.metadata.contains("refreshed_by"),
        "native browser OAuth refresh should use OAuth refresh orchestration");
    Expect(native_token_store.read_ref(native_refreshed.access_token_ref) == "curl-access",
        "native browser OAuth refresh should store refreshed access token");

    const auto persisted = agentos::PersistOAuthTokenSession(
        oauth_session_store,
        oauth_token_store,
        agentos::OAuthSessionPersistInput{
            .provider = agentos::AuthProviderId::gemini,
            .mode = agentos::AuthMode::browser_oauth,
            .profile_name = "pkce-persist",
            .account_label = "pkce@example.test",
            .token_response = token_response,
        });
    Expect(persisted.managed_by_agentos, "OAuth persisted session should be AgentOS managed");
    Expect(!persisted.managed_by_external_cli, "OAuth persisted session should not be external CLI managed");
    Expect(persisted.refresh_supported, "OAuth persisted session should be refreshable when refresh_token exists");
    Expect(oauth_token_store.read_ref(persisted.access_token_ref) == "access",
        "OAuth persisted session should store access token in credential store");
    Expect(oauth_token_store.read_ref(persisted.refresh_token_ref) == "refresh",
        "OAuth persisted session should store refresh token in credential store");

    agentos::SessionStore reloaded_oauth_store(workspace / "oauth_pkce" / "sessions.tsv");
    const auto reloaded_oauth = reloaded_oauth_store.find(agentos::AuthProviderId::gemini, "pkce-persist");
    Expect(reloaded_oauth.has_value(), "OAuth persisted session should reload from SessionStore");
    if (reloaded_oauth.has_value()) {
        Expect(reloaded_oauth->metadata.at("credential_source") == "oauth_pkce",
            "OAuth persisted session should record credential source metadata");
    }

    const auto refreshed_session = agentos::PersistOAuthRefreshSession(
        oauth_session_store,
        oauth_token_store,
        persisted,
        agentos::OAuthTokenResponse{
            .success = true,
            .access_token = "new-access",
            .token_type = "Bearer",
            .expires_in_seconds = 1200,
        });
    Expect(oauth_token_store.read_ref(refreshed_session.access_token_ref) == "new-access",
        "OAuth refresh persistence should update access token");
    Expect(refreshed_session.refresh_token_ref == persisted.refresh_token_ref,
        "OAuth refresh persistence should preserve existing refresh token when provider omits a new one");
    Expect(refreshed_session.metadata.contains("refreshed_by"),
        "OAuth refresh persistence should record refresh metadata");
    (void)oauth_token_store.delete_ref(orchestrated.access_token_ref);
    (void)oauth_token_store.delete_ref(orchestrated.refresh_token_ref);
    (void)oauth_token_store.delete_ref(persisted.access_token_ref);
    (void)oauth_token_store.delete_ref(persisted.refresh_token_ref);
#endif
}

void TestAuthRefreshSession(const std::filesystem::path& workspace) {
    agentos::SessionStore session_store(workspace / "auth_refresh" / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);

    auth_manager.register_provider(std::make_shared<agentos::GeminiAuthProviderAdapter>(session_store, token_store));

    const auto original_expiry = std::chrono::system_clock::now() - std::chrono::hours(1);
    session_store.save(agentos::AuthSession{
        .session_id = agentos::MakeAuthSessionId(agentos::AuthProviderId::gemini, agentos::AuthMode::browser_oauth, "smoke"),
        .provider = agentos::AuthProviderId::gemini,
        .mode = agentos::AuthMode::browser_oauth,
        .profile_name = "smoke",
        .account_label = "gemini-smoke",
        .managed_by_agentos = true,
        .managed_by_external_cli = false,
        .refresh_supported = true,
        .headless_compatible = true,
        .access_token_ref = "test-access-token-ref",
        .refresh_token_ref = "test-refresh-token-ref",
        .expires_at = original_expiry,
    });

    const auto refreshed = auth_manager.refresh(agentos::AuthProviderId::gemini, "smoke");
    Expect(refreshed.refresh_supported, "refreshed session should preserve refresh support");
    Expect(refreshed.expires_at > original_expiry, "refresh should extend the session expiry");
    Expect(refreshed.metadata.contains("refreshed_by"), "refresh should record adapter metadata");

    agentos::SessionStore reloaded_store(workspace / "auth_refresh" / "sessions.tsv");
    const auto reloaded = reloaded_store.find(agentos::AuthProviderId::gemini, "smoke");
    Expect(reloaded.has_value(), "refreshed session should be persisted");
    if (reloaded.has_value()) {
        Expect(reloaded->metadata.contains("refreshed_by"), "refreshed metadata should persist");
    }
}

void TestAuthDefaultProfileMapping(const std::filesystem::path& workspace) {
    SetEnvForTest("AGENTOS_TEST_QWEN_PROFILE_KEY", "profile-secret");

    const auto store_path = workspace / "auth_profiles" / "defaults.tsv";
    agentos::SessionStore session_store(workspace / "auth_profiles" / "sessions.tsv");
    agentos::AuthProfileStore profile_store(store_path);
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store, &profile_store);

    auth_manager.register_provider(std::make_shared<agentos::QwenAuthProviderAdapter>(session_store, token_store));
    auth_manager.set_default_profile(agentos::AuthProviderId::qwen, "team");
    Expect(auth_manager.default_profile(agentos::AuthProviderId::qwen) == "team", "auth manager should return workspace default profile");

    const auto session = auth_manager.login(
        agentos::AuthProviderId::qwen,
        agentos::AuthMode::api_key,
        {
            {"profile", auth_manager.default_profile(agentos::AuthProviderId::qwen)},
            {"api_key_env", "AGENTOS_TEST_QWEN_PROFILE_KEY"},
        });
    Expect(session.profile_name == "team", "auth login should use workspace default profile when supplied by CLI layer");

    agentos::AuthProfileStore reloaded_profile_store(store_path);
    agentos::AuthManager reloaded_manager(session_store, &reloaded_profile_store);
    Expect(reloaded_manager.default_profile(agentos::AuthProviderId::qwen) == "team", "workspace default profile should persist");
}

void TestAuthStatusReloadLogoutAndMissingEnv(const std::filesystem::path& workspace) {
    SetEnvForTest("AGENTOS_TEST_RELOAD_KEY", "reload-secret");

    const auto session_path = workspace / "auth_reload" / "sessions.tsv";
    agentos::SecureTokenStore token_store;
    {
        agentos::SessionStore session_store(session_path);
        agentos::AuthManager auth_manager(session_store);
        auth_manager.register_provider(std::make_shared<agentos::QwenAuthProviderAdapter>(session_store, token_store));
        (void)auth_manager.login(
            agentos::AuthProviderId::qwen,
            agentos::AuthMode::api_key,
            {
                {"profile", "reload"},
                {"api_key_env", "AGENTOS_TEST_RELOAD_KEY"},
            });
    }

    agentos::SessionStore reloaded_session_store(session_path);
    agentos::AuthManager reloaded_auth_manager(reloaded_session_store);
    reloaded_auth_manager.register_provider(std::make_shared<agentos::QwenAuthProviderAdapter>(reloaded_session_store, token_store));

    const auto reloaded_status = reloaded_auth_manager.status(agentos::AuthProviderId::qwen, "reload");
    Expect(reloaded_status.authenticated, "auth status should survive SessionStore reload");

    SetEnvForTest("AGENTOS_TEST_RELOAD_KEY", "");
    const auto missing_env_status = reloaded_auth_manager.status(agentos::AuthProviderId::qwen, "reload");
    Expect(!missing_env_status.authenticated, "auth status should fail when env ref is unavailable");
    Expect(missing_env_status.message == "credential reference is unavailable", "missing env ref should be reported clearly");

    reloaded_auth_manager.logout(agentos::AuthProviderId::qwen, "reload");
    const auto logged_out_status = reloaded_auth_manager.status(agentos::AuthProviderId::qwen, "reload");
    Expect(!logged_out_status.authenticated, "logout should remove the session");
    Expect(logged_out_status.message == "no session found", "logout status should report missing session");
}

void TestAuthCliSessionImportWithFixtures(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "auth_cli_session_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    const auto home_dir = isolated_workspace / "home";
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(home_dir);

    WriteCodexCliFixture(bin_dir, true);
    WriteClaudeCliFixture(bin_dir);
    WriteGeminiCliFixture(bin_dir);
    std::filesystem::create_directories(home_dir / ".gemini");
    {
        std::ofstream output(home_dir / ".gemini" / "oauth_creds.json", std::ios::binary);
        output << R"({"access_token":"fixture-access","refresh_token":"fixture-refresh"})";
    }

    const auto fixture_path = PrependPathForTest(bin_dir);
    ScopedEnvOverride path_override("PATH", fixture_path);
    ScopedEnvOverride home_override("HOME", home_dir.string());
    ScopedEnvOverride userprofile_override("USERPROFILE", home_dir.string());

    const auto session_path = isolated_workspace / "auth_cli" / "sessions.tsv";
    agentos::CliHost cli_host;
    agentos::SessionStore session_store(session_path);
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);
    auth_manager.register_provider(std::make_shared<agentos::OpenAiAuthProviderAdapter>(
        session_store, token_store, cli_host, isolated_workspace));
    auth_manager.register_provider(std::make_shared<agentos::AnthropicAuthProviderAdapter>(
        session_store, token_store, cli_host, isolated_workspace));
    auth_manager.register_provider(std::make_shared<agentos::GeminiAuthProviderAdapter>(
        session_store, token_store, cli_host, isolated_workspace));

    const auto detected_status = auth_manager.status(agentos::AuthProviderId::openai, "default");
    Expect(detected_status.authenticated, "Codex CLI fixture should be detected before import");
    Expect(detected_status.managed_by_external_cli, "Codex detected status should be marked as external CLI");
    Expect(detected_status.message == "external CLI session detected but not imported", "Codex detected status should distinguish probe from imported session");

    const auto codex_session = auth_manager.login(
        agentos::AuthProviderId::openai,
        agentos::AuthMode::cli_session_passthrough,
        {{"profile", "codex-fixture"}});
    Expect(codex_session.profile_name == "codex-fixture", "Codex CLI import should honor requested profile");
    Expect(codex_session.mode == agentos::AuthMode::cli_session_passthrough, "Codex CLI import should use cli-session mode");
    Expect(codex_session.managed_by_external_cli, "Codex CLI import should be marked as external CLI managed");
    Expect(codex_session.access_token_ref == "external-cli:codex", "Codex CLI import should persist external CLI token ref");
    Expect(codex_session.metadata.at("probe").find("Logged in") != std::string::npos, "Codex CLI import should preserve probe output");

    const auto codex_status = auth_manager.status(agentos::AuthProviderId::openai, "codex-fixture");
    Expect(codex_status.authenticated, "Imported Codex CLI session should authenticate");
    Expect(codex_status.message == "session available", "Imported Codex CLI status should read from SessionStore");

    const auto claude_session = auth_manager.login(
        agentos::AuthProviderId::anthropic,
        agentos::AuthMode::cli_session_passthrough,
        {{"profile", "claude-fixture"}});
    Expect(claude_session.profile_name == "claude-fixture", "Claude CLI import should honor requested profile");
    Expect(claude_session.managed_by_external_cli, "Claude CLI import should be marked as external CLI managed");
    Expect(claude_session.access_token_ref == "external-cli:claude", "Claude CLI import should persist external CLI token ref");
    Expect(claude_session.metadata.at("probe") == "loggedIn=true", "Claude CLI import should preserve normalized probe metadata");

    agentos::SessionStore reloaded_store(session_path);
    Expect(reloaded_store.find(agentos::AuthProviderId::openai, "codex-fixture").has_value(), "Imported Codex CLI session should persist");
    Expect(reloaded_store.find(agentos::AuthProviderId::anthropic, "claude-fixture").has_value(), "Imported Claude CLI session should persist");
    const auto gemini_session = auth_manager.login(
        agentos::AuthProviderId::gemini,
        agentos::AuthMode::browser_oauth,
        {{"profile", "gemini-fixture"}});
    Expect(gemini_session.profile_name == "gemini-fixture", "Gemini OAuth import should honor requested profile");
    Expect(gemini_session.mode == agentos::AuthMode::browser_oauth, "Gemini OAuth import should preserve browser_oauth mode");
    Expect(gemini_session.managed_by_external_cli, "Gemini OAuth import should be marked as external CLI managed");
    Expect(gemini_session.access_token_ref == "external-cli:gemini", "Gemini OAuth import should persist external CLI token ref");
    Expect(gemini_session.metadata.at("probe") == "oauth-file-present", "Gemini OAuth import should preserve normalized probe metadata");
}

void TestAuthCliSessionUnavailableWithFixture(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "auth_cli_unavailable_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    const auto home_dir = isolated_workspace / "home";
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(home_dir);

    WriteCodexCliFixture(bin_dir, false);

    const auto fixture_path = PrependPathForTest(bin_dir);
    ScopedEnvOverride path_override("PATH", fixture_path);
    ScopedEnvOverride home_override("HOME", home_dir.string());
    ScopedEnvOverride userprofile_override("USERPROFILE", home_dir.string());

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "auth_cli_unavailable" / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);
    auth_manager.register_provider(std::make_shared<agentos::OpenAiAuthProviderAdapter>(
        session_store, token_store, cli_host, isolated_workspace));

    bool unavailable = false;
    try {
        (void)auth_manager.login(
            agentos::AuthProviderId::openai,
            agentos::AuthMode::cli_session_passthrough,
            {{"profile", "missing-cli-session"}});
    } catch (const std::exception& error) {
        unavailable = std::string(error.what()) == "CliSessionUnavailable";
    }
    Expect(unavailable, "Codex CLI import should fail clearly when fixture reports no login");

    const auto status = auth_manager.status(agentos::AuthProviderId::openai, "missing-cli-session");
    Expect(!status.authenticated, "Unavailable Codex CLI fixture should not authenticate status");
    Expect(status.message == "no session found", "Unavailable Codex CLI fixture should leave no imported session");
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

void TestAuthOAuthUnavailableWithoutGeminiCliSession(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "auth_oauth_unavailable";
    const auto home_dir = isolated_workspace / "home";
    std::filesystem::create_directories(home_dir);
    ScopedEnvOverride home_override("HOME", home_dir.string());
    ScopedEnvOverride userprofile_override("USERPROFILE", home_dir.string());

    agentos::SessionStore session_store(isolated_workspace / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);

    auth_manager.register_provider(std::make_shared<agentos::GeminiAuthProviderAdapter>(session_store, token_store));

    bool unavailable = false;
    try {
        (void)auth_manager.login(agentos::AuthProviderId::gemini, agentos::AuthMode::browser_oauth, {{"profile", "smoke"}});
    } catch (const std::exception& error) {
        unavailable = std::string(error.what()) == "BrowserOAuthUnavailable";
    }

    Expect(unavailable, "Gemini browser OAuth should fail clearly when no Gemini CLI OAuth session is available");
}

void TestAuthGeminiCloudAdcLogin(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "auth_cloud_adc";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);
    const auto bin_dir = isolated_workspace / "bin";
    const auto home_dir = isolated_workspace / "home";
    const auto adc_dir = home_dir / ".config" / "gcloud";
    std::filesystem::create_directories(bin_dir);
    std::filesystem::create_directories(adc_dir);
    {
        std::ofstream output(adc_dir / "application_default_credentials.json", std::ios::binary);
        output << R"({"type":"authorized_user","client_id":"fixture","refresh_token":"fixture-refresh"})";
    }
    WriteGcloudCliFixture(bin_dir);

    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    ScopedEnvOverride home_override("HOME", home_dir.string());
    ScopedEnvOverride userprofile_override("USERPROFILE", home_dir.string());

    agentos::CliHost cli_host;
    agentos::SessionStore session_store(isolated_workspace / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);
    auth_manager.register_provider(std::make_shared<agentos::GeminiAuthProviderAdapter>(
        session_store, token_store, cli_host, isolated_workspace));

    const auto session = auth_manager.login(
        agentos::AuthProviderId::gemini,
        agentos::AuthMode::cloud_adc,
        {{"profile", "adc"}});

    Expect(session.mode == agentos::AuthMode::cloud_adc, "Gemini ADC login should persist cloud_adc mode");
    Expect(session.access_token_ref == "external-cli:gcloud-adc", "Gemini ADC login should use gcloud ADC token ref");
    Expect(session.refresh_supported, "Gemini ADC login should be refreshable through gcloud token minting");

    const auto status = auth_manager.status(agentos::AuthProviderId::gemini, "adc");
    Expect(status.authenticated, "Gemini ADC status should authenticate after login");
    Expect(status.managed_by_external_cli, "Gemini ADC status should be marked external-CLI managed");
}

void TestAuthMissingSessionAndUnregisteredProviderFailures(const std::filesystem::path& workspace) {
    agentos::SessionStore session_store(workspace / "auth_failure_paths" / "sessions.tsv");
    agentos::SecureTokenStore token_store;
    agentos::AuthManager auth_manager(session_store);

    auth_manager.register_provider(std::make_shared<agentos::GeminiAuthProviderAdapter>(session_store, token_store));

    bool missing_session = false;
    try {
        (void)auth_manager.refresh(agentos::AuthProviderId::gemini, "missing");
    } catch (const std::exception& error) {
        missing_session = std::string(error.what()) == "SessionNotFound";
    }
    Expect(missing_session, "auth refresh should fail clearly when the requested session is missing");

    bool provider_not_registered = false;
    try {
        (void)auth_manager.status(agentos::AuthProviderId::openai, "default");
    } catch (const std::exception& error) {
        provider_not_registered = std::string(error.what()) == "ProviderNotRegistered";
    }
    Expect(provider_not_registered, "auth status should fail clearly for unregistered providers");
}

void TestSecureTokenStoreInMemoryBackend(const std::filesystem::path& workspace) {
    (void)workspace;

    agentos::SecureTokenStore store;
    auto in_memory = agentos::SecureTokenStore::MakeInMemoryBackendForTesting();
    store.set_backend_for_testing(in_memory);

    const auto status = store.status();
    Expect(status.backend_name == "in-memory", "in-memory backend should report its name in status");
    Expect(!status.system_keychain_backed, "in-memory backend should not claim system keychain support");
    Expect(status.dev_only, "in-memory backend should be marked dev-only");
    Expect(store.backend_name() == "in-memory", "backend_name accessor should match status backend");

    // Env-ref reads still work even when a custom backend is installed.
    SetEnvForTest("AGENTOS_TEST_INMEM_KEY", "env-secret");
    const auto env_ref = store.make_env_ref("AGENTOS_TEST_INMEM_KEY");
    Expect(env_ref == "env:AGENTOS_TEST_INMEM_KEY", "make_env_ref should produce env-prefixed refs");
    Expect(store.read_ref(env_ref) == "env-secret", "in-memory backend should not interfere with env: refs");
    Expect(store.ref_available(env_ref), "ref_available should follow read_ref for env refs");
    Expect(!store.delete_ref(env_ref), "delete_ref should refuse to remove env-backed refs");

    // Round-trip a managed token through the in-memory backend.
    const auto managed_ref = store.write_managed_token("anthropic", "smoke", "access", "managed-secret");
    Expect(managed_ref.rfind("memtoken:", 0) == 0, "in-memory backend should use the memtoken: prefix");
    Expect(store.read_ref(managed_ref) == "managed-secret", "in-memory backend should read what it wrote");
    Expect(store.ref_available(managed_ref), "in-memory managed refs should be available after write");

    // make_managed_ref should produce the same ref as write_managed_token.
    const auto computed_ref = store.make_managed_ref("anthropic", "smoke", "access");
    Expect(computed_ref == managed_ref, "make_managed_ref should match what write_managed_token returns");

    // Empty token values are rejected.
    bool rejected_empty = false;
    try {
        (void)store.write_managed_token("anthropic", "smoke", "access", "");
    } catch (const std::exception& error) {
        rejected_empty = std::string(error.what()).find("PolicyDenied") != std::string::npos;
    }
    Expect(rejected_empty, "write_managed_token should reject empty values with PolicyDenied");

    // Delete should remove the entry from the backend.
    Expect(store.delete_ref(managed_ref), "delete_ref should report success on existing in-memory ref");
    Expect(!store.ref_available(managed_ref), "deleted in-memory refs should no longer be available");
    Expect(!store.delete_ref(managed_ref), "deleting a missing in-memory ref should report false");

    // Unknown ref prefixes return nullopt and don't crash.
    Expect(!store.read_ref("unknown-prefix:foo").has_value(), "unknown ref prefixes should return nullopt");
    Expect(!store.delete_ref("unknown-prefix:foo"), "delete_ref should return false for unknown prefixes");

    // Restoring the platform default backend is supported.
    store.set_backend_for_testing(nullptr);
    const auto restored = store.status().backend_name;
#ifdef _WIN32
    Expect(restored == "windows-credential-manager", "platform default backend on Windows should be Credential Manager");
#elif defined(__APPLE__)
    Expect(restored == "macos-keychain", "platform default backend on macOS should be Keychain");
#else
    Expect(restored == "env-ref-only" || restored == "linux-secret-service",
        "platform default backend on Linux should be env-ref-only or linux-secret-service");
#endif

    ClearEnvForTest("AGENTOS_TEST_INMEM_KEY");
}

void TestOAuthDefaultsCoverageAndConfigOverride(const std::filesystem::path& workspace) {
    // Builtin defaults: gemini is supported with origin=builtin; openai,
    // anthropic, qwen are stubbed with origin=stub.
    {
        const auto gemini = agentos::OAuthDefaultsForProvider(agentos::AuthProviderId::gemini);
        Expect(gemini.supported, "gemini should have builtin OAuth defaults");
        Expect(gemini.origin == "builtin", "gemini defaults origin should be builtin");
        Expect(!gemini.note.empty(), "gemini defaults should carry a human-readable note");
        Expect(!gemini.token_endpoint.empty(), "gemini token endpoint should be populated");
    }
    for (const auto provider : {
             agentos::AuthProviderId::openai,
             agentos::AuthProviderId::anthropic,
             agentos::AuthProviderId::qwen,
         }) {
        const auto defaults = agentos::OAuthDefaultsForProvider(provider);
        Expect(!defaults.supported, "non-gemini providers should be unsupported by default");
        Expect(defaults.origin == "stub", "non-gemini providers should be marked as stub");
        Expect(!defaults.note.empty(), "stub providers should explain why OAuth is unavailable");
        Expect(defaults.authorization_endpoint.empty(), "stub providers should not expose endpoints");
        Expect(defaults.token_endpoint.empty(), "stub providers should not expose endpoints");
    }

    // Loading a config file should mark loaded entries with origin=config.
    const auto config_path = workspace / "runtime" / "auth_oauth_providers_test.tsv";
    std::filesystem::create_directories(config_path.parent_path());
    {
        std::ofstream output(config_path, std::ios::binary);
        output << "# AgentOS OAuth provider defaults override (test)\n";
        output << "openai\thttps://example.test/auth\thttps://example.test/token\topenid,profile\n";
        output << "anthropic\t\t\t\n";  // missing endpoints
    }

    const auto loaded = agentos::LoadOAuthProviderDefaultsFromFile(config_path);
    Expect(loaded.size() == 2, "loader should return all parsed rows including invalid ones");
    const auto openai_it = loaded.find(agentos::AuthProviderId::openai);
    Expect(openai_it != loaded.end(), "loader should include openai override");
    Expect(openai_it->second.origin == "config", "loaded entries should carry origin=config");
    Expect(openai_it->second.supported, "openai override with full endpoints should be marked supported");

    const auto anthropic_it = loaded.find(agentos::AuthProviderId::anthropic);
    Expect(anthropic_it != loaded.end(), "loader should include anthropic override row");
    Expect(!anthropic_it->second.supported, "anthropic override with no endpoints should not be supported");

    // Merging a config override on top of a stub builtin should mark the
    // result origin=config and refresh the stub note.
    const auto merged = agentos::MergeOAuthProviderDefaults(
        agentos::OAuthDefaultsForProvider(agentos::AuthProviderId::openai),
        openai_it->second);
    Expect(merged.supported, "merged openai defaults should be supported");
    Expect(merged.origin == "config", "merged defaults should report origin=config");
    Expect(merged.token_endpoint == "https://example.test/token", "merged defaults should pick up override token endpoint");
    Expect(merged.note.find("override") != std::string::npos,
        "merged defaults should reset the stub note when config supplies endpoints");

    // Single-provider lookup helper.
    const auto override_only = agentos::LoadOAuthProviderDefaultsFromFile(config_path, agentos::AuthProviderId::openai);
    Expect(override_only.has_value(), "single-provider loader should find openai override");
    Expect(override_only->origin == "config", "single-provider loader should preserve origin=config");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestAuthApiKeySession(workspace);
    TestOAuthPkceScaffold(workspace);
    TestAuthRefreshSession(workspace);
    TestAuthDefaultProfileMapping(workspace);
    TestAuthStatusReloadLogoutAndMissingEnv(workspace);
    TestAuthCliSessionImportWithFixtures(workspace);
    TestAuthCliSessionUnavailableWithFixture(workspace);
    TestAuthUnsupportedMode(workspace);
    TestAuthOAuthUnavailableWithoutGeminiCliSession(workspace);
    TestAuthGeminiCloudAdcLogin(workspace);
    TestAuthMissingSessionAndUnregisteredProviderFailures(workspace);
    TestSecureTokenStoreInMemoryBackend(workspace);
    TestOAuthDefaultsCoverageAndConfigOverride(workspace);

    if (failures != 0) {
        std::cerr << failures << " auth test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_auth_tests passed\n";
    return 0;
}
