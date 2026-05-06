#include "auth/auth_manager.hpp"
#include "auth/auth_login_flow.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "auth/oauth_pkce.hpp"
#include "auth/provider_adapters.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "cli/auth_interactive.hpp"
#include "hosts/cli/cli_host.hpp"
#include "utils/secure_random.hpp"
#include "test_command_fixtures.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
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

// Writes a curl fixture that records its argv to `args_file` AND, for any
// argument starting with `@`, appends the dereferenced file contents to the
// sibling path `<args_file>.deref`. This mirrors how real curl reads
// `--data @file` / `-H @file` while letting tests inspect both the argv
// (to verify secrets do NOT appear there) and the request payload (to
// verify it still contains the expected grant_type / refresh_token / etc.).
void WriteOAuthCurlFixture(const std::filesystem::path& bin_dir, const std::filesystem::path& args_file) {
    const auto deref_file = args_file.string() + ".deref";
#ifdef _WIN32
    WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo %* > \"" + args_file.generic_string() + "\"\n"
        "type nul > \"" + deref_file + "\"\n"
        ":next\n"
        "set \"arg=%~1\"\n"
        "if \"%arg%\"==\"\" goto done\n"
        "if \"%arg:~0,1%\"==\"@\" type \"%arg:~1%\" >> \"" + deref_file + "\"\n"
        "shift\n"
        "goto next\n"
        ":done\n"
        "echo {\"access_token\":\"curl-access\",\"refresh_token\":\"curl-refresh\",\"token_type\":\"Bearer\",\"expires_in\":1800}\n"
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
        "Host: 127.0.0.1:" + std::to_string(port) + "\r\n"
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

std::string SendHttpGetWithHostForTest(const int port, const std::string& target, const std::string& host_header) {
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
        "Host: " + host_header + "\r\n"
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

void TestOAuthListenerRequestValidation(const std::filesystem::path& workspace) {
    (void)workspace;

    Expect(agentos::ExtractRedirectUriPathForTest("http://127.0.0.1:48177/callback") == "/callback",
        "ExtractRedirectUriPath should return path component");
    Expect(agentos::ExtractRedirectUriPathForTest("http://127.0.0.1:48177") == "/",
        "ExtractRedirectUriPath should default to / when path missing");
    Expect(agentos::ExtractRedirectUriPathForTest("http://127.0.0.1:48177/cb?x=1") == "/cb",
        "ExtractRedirectUriPath should strip query string");

    Expect(agentos::TargetPathOnlyForTest("/callback?code=X&state=Y") == "/callback",
        "TargetPathOnly should strip query");
    Expect(agentos::TargetPathOnlyForTest("/callback") == "/callback",
        "TargetPathOnly should pass through path-only target");

    const auto host = agentos::ExtractHttpHostHeaderForTest(
        "GET /x HTTP/1.1\r\nHost: 127.0.0.1:48177\r\nConnection: close\r\n\r\n");
    Expect(host.has_value() && *host == "127.0.0.1:48177",
        "ExtractHttpHostHeader should parse Host header value");
    const auto missing_host = agentos::ExtractHttpHostHeaderForTest(
        "GET /x HTTP/1.1\r\nConnection: close\r\n\r\n");
    Expect(!missing_host.has_value(), "ExtractHttpHostHeader should return nullopt when Host missing");

    Expect(agentos::HostHeaderMatchesLoopbackForTest("127.0.0.1:48177", 48177),
        "HostHeaderMatchesLoopback should accept 127.0.0.1 with matching port");
    Expect(agentos::HostHeaderMatchesLoopbackForTest("localhost:48177", 48177),
        "HostHeaderMatchesLoopback should accept localhost with matching port");
    Expect(!agentos::HostHeaderMatchesLoopbackForTest("127.0.0.1", 48177),
        "HostHeaderMatchesLoopback should reject Host without port");
    Expect(!agentos::HostHeaderMatchesLoopbackForTest("example.com:48177", 48177),
        "HostHeaderMatchesLoopback should reject non-loopback hosts");
    Expect(!agentos::HostHeaderMatchesLoopbackForTest("127.0.0.1:48180", 48177),
        "HostHeaderMatchesLoopback should reject mismatched port");

    const auto start = agentos::CreateOAuthPkceStart(agentos::OAuthPkceStartRequest{
        .provider = agentos::AuthProviderId::gemini,
        .profile_name = "listener-paths",
        .authorization_endpoint = "https://accounts.example.test/o/oauth2/v2/auth",
        .client_id = "client-id",
        .redirect_uri = "http://127.0.0.1:48177/callback",
        .state = "fixed-state",
        .code_verifier = "fixed-verifier-value-with-enough-length-1234567890",
    });

    {
        const auto port = FindFreeLoopbackPortForTest();
        auto listener = std::async(std::launch::async, [&]() {
            return agentos::ListenForOAuthCallbackOnce(agentos::OAuthCallbackListenRequest{
                .start = start,
                .port = port,
                .timeout_ms = 5000,
            });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto response = SendHttpGetForTest(port, "/garbage?state=fixed-state&code=X");
        const auto result = listener.get();
        Expect(!result.success, "OAuth listener must reject callbacks with non-redirect_uri path");
        Expect(result.error == "InvalidCallbackRequest",
            "OAuth listener must surface InvalidCallbackRequest for path mismatch");
        Expect(response.find("400 Bad Request") != std::string::npos,
            "OAuth listener should respond 400 to a path-mismatched GET");
    }

    {
        const auto port = FindFreeLoopbackPortForTest();
        auto listener = std::async(std::launch::async, [&]() {
            return agentos::ListenForOAuthCallbackOnce(agentos::OAuthCallbackListenRequest{
                .start = start,
                .port = port,
                .timeout_ms = 5000,
            });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto response = SendHttpGetWithHostForTest(
            port,
            "/callback?state=fixed-state&code=X",
            "evil.example.com:" + std::to_string(port));
        const auto result = listener.get();
        Expect(!result.success, "OAuth listener must reject callbacks with non-loopback Host header");
        Expect(result.error == "InvalidCallbackRequest",
            "OAuth listener must surface InvalidCallbackRequest for Host mismatch");
        Expect(response.find("400 Bad Request") != std::string::npos,
            "OAuth listener should respond 400 to a Host-mismatched GET");
    }
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
#elif defined(AGENTOS_HAVE_LIBSECRET)
    Expect(token_store_status.backend_name == "linux-secret-service",
        "secure token store should report Linux Secret Service backend when libsecret is available");
    Expect(token_store_status.system_keychain_backed,
        "secure token store should claim system credential support when libsecret is available");
    Expect(!token_store_status.dev_only, "Linux Secret Service credential store should not be marked dev-only");
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

void TestAuthLoginFlowModulesWithFixtureStores(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "auth_login_flow_modules";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    SetEnvForTest("AGENTOS_FLOW_TEST_KEY", "flow-secret");

    agentos::SessionStore session_store(isolated_workspace / "sessions.tsv");
    agentos::SecureTokenStore token_store(agentos::SecureTokenStore::MakeInMemoryBackendForTesting());
    const auto bin_dir = isolated_workspace / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto curl_args_file = isolated_workspace / "flow_curl_args.txt";
    WriteOAuthCurlFixture(bin_dir, curl_args_file);
    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));
    agentos::CliHost cli_host;
    const agentos::AuthProviderDescriptor descriptor{
        .provider = agentos::AuthProviderId::gemini,
        .provider_name = "gemini",
        .supported_modes = {
            agentos::AuthMode::api_key,
            agentos::AuthMode::cli_session_passthrough,
            agentos::AuthMode::browser_oauth,
            agentos::AuthMode::cloud_adc,
        },
        .browser_login_supported = true,
        .headless_supported = true,
        .refresh_token_supported = true,
        .cli_session_passthrough_supported = true,
    };

    agentos::AuthLoginFlowContext context{
        .descriptor = descriptor,
        .session_store = session_store,
        .token_store = token_store,
        .cli_host = &cli_host,
        .workspace_path = isolated_workspace,
        .default_api_key_env = "AGENTOS_FLOW_TEST_KEY",
        .probe_cli_session = []() {
            return agentos::AuthSession{
                .session_id = "external-session",
                .provider = agentos::AuthProviderId::gemini,
                .mode = agentos::AuthMode::cli_session_passthrough,
                .profile_name = "external",
                .account_label = "fixture-cli",
                .managed_by_agentos = false,
                .managed_by_external_cli = true,
                .refresh_supported = false,
                .headless_compatible = false,
                .access_token_ref = "external-cli:fixture",
                .expires_at = std::chrono::system_clock::now() + std::chrono::hours(24),
                .metadata = {{"provider", "gemini"}, {"cli", "fixture"}},
            };
        },
        .cloud_adc_available = []() {
            return true;
        },
    };

    const auto api_key = agentos::LoginWithApiKeyEnvRef(context, "api", {});
    Expect(api_key.mode == agentos::AuthMode::api_key,
        "Auth Login Flow API-key module should create api_key sessions");
    Expect(api_key.access_token_ref == "env:AGENTOS_FLOW_TEST_KEY",
        "Auth Login Flow API-key module should store an env ref");

    const auto cli_session = agentos::LoginWithCliSessionPassthrough(context, "cli");
    Expect(cli_session.mode == agentos::AuthMode::cli_session_passthrough,
        "Auth Login Flow CLI module should preserve cli-session mode");
    Expect(cli_session.profile_name == "cli",
        "Auth Login Flow CLI module should apply the requested profile");
    Expect(cli_session.session_id == agentos::MakeAuthSessionId(
            agentos::AuthProviderId::gemini,
            agentos::AuthMode::cli_session_passthrough,
            "cli"),
        "Auth Login Flow CLI module should regenerate the session id for the profile");

    const auto browser_session = agentos::LoginWithBrowserOAuthPkce(context, "browser", {});
    Expect(browser_session.mode == agentos::AuthMode::browser_oauth,
        "Auth Login Flow browser module should use the fixture probe fallback");
    Expect(browser_session.profile_name == "browser",
        "Auth Login Flow browser module should apply the requested profile");

    const auto native_browser_session = agentos::LoginWithBrowserOAuthPkce(
        context,
        "native-browser",
        {
            {"callback_url", "http://127.0.0.1:48177/callback?code=native-code&state=native-state"},
            {"state", "native-state"},
            {"code_verifier", "native-verifier-secret"},
            {"redirect_uri", "http://127.0.0.1:48177/callback"},
            {"client_id", "native-client"},
            {"token_endpoint", "https://oauth2.example.test/token"},
            {"account_label", "native@example.test"},
        });
    Expect(native_browser_session.mode == agentos::AuthMode::browser_oauth,
        "Auth Login Flow browser module should complete native PKCE sessions when callback options are injected");
    Expect(native_browser_session.managed_by_agentos,
        "Auth Login Flow native PKCE sessions should be AgentOS managed");
    Expect(token_store.read_ref(native_browser_session.access_token_ref) == "curl-access",
        "Auth Login Flow native PKCE session should store the exchanged access token in the injected token store");
    Expect(token_store.read_ref(native_browser_session.refresh_token_ref) == "curl-refresh",
        "Auth Login Flow native PKCE session should store the exchanged refresh token in the injected token store");
    Expect(native_browser_session.metadata.at("credential_source") == "oauth_pkce",
        "Auth Login Flow native PKCE session should record the credential source");

    const auto adc_session = agentos::LoginWithCloudAdc(context, "adc");
    Expect(adc_session.mode == agentos::AuthMode::cloud_adc,
        "Auth Login Flow ADC module should create cloud_adc sessions when availability is injected");
    Expect(adc_session.access_token_ref == "external-cli:gcloud-adc",
        "Auth Login Flow ADC module should use the gcloud ADC token ref");

    auto refreshable = adc_session;
    refreshable.metadata["credential_source"] = "google_adc";
    refreshable.expires_at = std::chrono::system_clock::now() - std::chrono::minutes(1);
    const auto refreshed = agentos::RefreshAuthLoginFlow(context, refreshable);
    Expect(refreshed.metadata.at("refreshed_by") == "static-adapter",
        "Auth Login Flow refresh module should refresh non-native sessions through the static fallback");
    Expect(refreshed.expires_at > refreshable.expires_at,
        "Auth Login Flow refresh module should advance the expiry");

    const auto native_refreshed = agentos::RefreshAuthLoginFlow(context, native_browser_session);
    Expect(native_refreshed.metadata.at("refreshed_by") == "oauth_refresh_token",
        "Auth Login Flow refresh module should route native PKCE sessions through OAuth refresh");
    Expect(token_store.read_ref(native_refreshed.access_token_ref) == "curl-access",
        "Auth Login Flow native refresh should store the refreshed access token in the injected token store");
}

void TestSecureRandomBytesContract(const std::filesystem::path& workspace) {
    (void)workspace;
    const auto first = agentos::SecureRandomBytes(32);
    const auto second = agentos::SecureRandomBytes(32);
    Expect(first.size() == 32, "SecureRandomBytes should fill the requested length");
    Expect(second.size() == 32, "SecureRandomBytes should fill the requested length");
    Expect(first != second, "SecureRandomBytes consecutive draws must differ (non-deterministic CSPRNG)");
    bool all_zero = true;
    for (const auto byte : first) {
        if (byte != 0) {
            all_zero = false;
            break;
        }
    }
    Expect(!all_zero, "SecureRandomBytes must not return all-zero output");

    const auto empty = agentos::SecureRandomBytes(0);
    Expect(empty.empty(), "SecureRandomBytes(0) should return empty without throwing");
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
    const auto deref_file = std::filesystem::path(args_file.string() + ".deref");
    WriteOAuthCurlFixture(bin_dir, args_file);
    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    auto read_text_file = [](const std::filesystem::path& path) -> std::string {
        std::ifstream input(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    };

    agentos::CliHost cli_host;
    const auto exchanged = agentos::ExecuteOAuthTokenExchange(
        cli_host,
        agentos::OAuthTokenRequestInput{
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .redirect_uri = "http://127.0.0.1:48177/callback",
            .code = "auth-code",
            .code_verifier = "verifier-secret-must-not-leak",
        },
        workspace);
    Expect(exchanged.success, "OAuth token exchange should execute through the controlled CLI host");
    Expect(exchanged.access_token == "curl-access", "OAuth token exchange should parse access token from curl response");
    Expect(exchanged.refresh_token == "curl-refresh", "OAuth token exchange should parse refresh token from curl response");
    Expect(exchanged.expires_in_seconds == 1800, "OAuth token exchange should parse token expiry from curl response");
    const std::string curl_args = read_text_file(args_file);
    const std::string curl_body = read_text_file(deref_file);
    Expect(curl_args.find("-X POST") != std::string::npos, "OAuth token exchange should use POST");
    Expect(curl_args.find("Content-Type: application/x-www-form-urlencoded") != std::string::npos,
        "OAuth token exchange should send form content type");
    Expect(curl_body.find("grant_type=authorization_code") != std::string::npos,
        "OAuth token exchange should send authorization_code grant in deref'd body");

    // Phase 1.1: code_verifier / authorization code MUST NOT appear on argv;
    // they belong inside the @file body, not on /proc/<pid>/cmdline.
    Expect(curl_args.find("code_verifier=") == std::string::npos,
        "OAuth token exchange must NOT pass code_verifier as argv");
    Expect(curl_args.find("verifier-secret-must-not-leak") == std::string::npos,
        "OAuth token exchange must NOT leak code_verifier value as argv");
    Expect(curl_args.find("code=auth-code") == std::string::npos,
        "OAuth token exchange must NOT pass authorization code as argv");
    Expect(curl_args.find("@") != std::string::npos,
        "OAuth token exchange must reference its body via curl @file syntax");
    Expect(curl_body.find("code_verifier=verifier-secret-must-not-leak") != std::string::npos,
        "OAuth token exchange should still send the code_verifier inside the deref'd body");

    const auto refreshed = agentos::ExecuteOAuthRefreshTokenExchange(
        cli_host,
        agentos::OAuthRefreshTokenRequestInput{
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .refresh_token = "refresh-token-must-not-leak",
        },
        workspace);
    Expect(refreshed.success, "OAuth refresh token exchange should execute through the controlled CLI host");
    Expect(refreshed.access_token == "curl-access", "OAuth refresh token exchange should parse refreshed access token");
    const std::string refresh_curl_args = read_text_file(args_file);
    const std::string refresh_curl_body = read_text_file(deref_file);
    Expect(refresh_curl_body.find("grant_type=refresh_token") != std::string::npos,
        "OAuth refresh token exchange should send refresh_token grant in deref'd body");

    // Phase 1.1: refresh_token MUST NOT appear on argv.
    Expect(refresh_curl_args.find("refresh_token=") == std::string::npos,
        "OAuth refresh token exchange must NOT pass refresh_token as argv");
    Expect(refresh_curl_args.find("refresh-token-must-not-leak") == std::string::npos,
        "OAuth refresh token exchange must NOT leak refresh_token value as argv");
    Expect(refresh_curl_args.find("@") != std::string::npos,
        "OAuth refresh token exchange must reference its body via curl @file syntax");
    Expect(refresh_curl_body.find("refresh_token=refresh-token-must-not-leak") != std::string::npos,
        "OAuth refresh token exchange should still send the refresh_token inside the deref'd body");

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
    {
        const auto openai = agentos::OAuthDefaultsForProvider(agentos::AuthProviderId::openai);
        Expect(openai.supported, "openai should have builtin OAuth defaults");
        Expect(openai.origin == "builtin", "openai defaults origin should be builtin");
        Expect(!openai.note.empty(), "openai defaults should carry a human-readable note");
        Expect(!openai.token_endpoint.empty(), "openai token endpoint should be populated");
    }
    for (const auto provider : {
             agentos::AuthProviderId::anthropic,
             agentos::AuthProviderId::qwen,
         }) {
        const auto defaults = agentos::OAuthDefaultsForProvider(provider);
        Expect(!defaults.supported, "non-gemini/openai providers should be unsupported by default");
        Expect(defaults.origin == "stub", "non-gemini/openai providers should be marked as stub");
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

    // Merging a config override on top of the builtin openai defaults should
    // mark the result origin=config and keep the override endpoints.
    const auto merged = agentos::MergeOAuthProviderDefaults(
        agentos::OAuthDefaultsForProvider(agentos::AuthProviderId::openai),
        openai_it->second);
    Expect(merged.supported, "merged openai defaults should be supported");
    Expect(merged.origin == "config", "merged defaults should report origin=config");
    Expect(merged.token_endpoint == "https://example.test/token", "merged defaults should pick up override token endpoint");

    // Single-provider lookup helper.
    const auto override_only = agentos::LoadOAuthProviderDefaultsFromFile(config_path, agentos::AuthProviderId::openai);
    Expect(override_only.has_value(), "single-provider loader should find openai override");
    Expect(override_only->origin == "config", "single-provider loader should preserve origin=config");
}

// Phase 1.1 dedicated leak guard. Confirms that OAuth token-exchange and
// refresh-token-exchange route their form bodies (which contain
// `code_verifier`, `code`, and `refresh_token`) through curl's `--data
// @file` syntax rather than spilling them onto argv.
void TestOAuthExchangeDoesNotLeakSecretsOnArgv(const std::filesystem::path& workspace) {
    const auto isolated = workspace / "oauth_argv_leak_guard";
    std::filesystem::remove_all(isolated);
    std::filesystem::create_directories(isolated);
    const auto bin_dir = isolated / "bin";
    std::filesystem::create_directories(bin_dir);
    const auto args_file = isolated / "leak_guard_args.txt";
    const auto deref_file = std::filesystem::path(args_file.string() + ".deref");
    WriteOAuthCurlFixture(bin_dir, args_file);
    ScopedEnvOverride path_override("PATH", PrependPathForTest(bin_dir));

    auto read_text = [](const std::filesystem::path& path) -> std::string {
        std::ifstream input(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    };

    constexpr const char* kVerifier = "phase11-pkce-verifier-must-not-leak-on-argv";
    constexpr const char* kAuthCode = "phase11-auth-code-secret-arg";
    constexpr const char* kRefreshToken = "phase11-refresh-token-must-not-leak-on-argv";

    agentos::CliHost cli_host;

    // Token exchange: code_verifier + authorization code must not appear on argv.
    const auto exchanged = agentos::ExecuteOAuthTokenExchange(
        cli_host,
        agentos::OAuthTokenRequestInput{
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .redirect_uri = "http://127.0.0.1:48177/callback",
            .code = kAuthCode,
            .code_verifier = kVerifier,
        },
        isolated);
    Expect(exchanged.success, "Phase 1.1 leak-guard token exchange should succeed");

    const auto exchange_args = read_text(args_file);
    const auto exchange_body = read_text(deref_file);
    Expect(exchange_args.find(kVerifier) == std::string::npos,
        "Phase 1.1: ExecuteOAuthTokenExchange must NOT spill code_verifier onto curl argv");
    Expect(exchange_args.find(kAuthCode) == std::string::npos,
        "Phase 1.1: ExecuteOAuthTokenExchange must NOT spill authorization code onto curl argv");
    Expect(exchange_args.find("code_verifier=") == std::string::npos,
        "Phase 1.1: ExecuteOAuthTokenExchange must NOT pass code_verifier= as argv");
    Expect(exchange_args.find("@") != std::string::npos,
        "Phase 1.1: ExecuteOAuthTokenExchange must reference body via curl @file syntax");
    Expect(exchange_body.find(kVerifier) != std::string::npos,
        "Phase 1.1: code_verifier should still reach the request body via the deref'd file");
    Expect(exchange_body.find(kAuthCode) != std::string::npos,
        "Phase 1.1: authorization code should still reach the request body via the deref'd file");

    // Refresh exchange: refresh_token must not appear on argv.
    const auto refreshed = agentos::ExecuteOAuthRefreshTokenExchange(
        cli_host,
        agentos::OAuthRefreshTokenRequestInput{
            .token_endpoint = "https://oauth2.example.test/token",
            .client_id = "client-id",
            .refresh_token = kRefreshToken,
        },
        isolated);
    Expect(refreshed.success, "Phase 1.1 leak-guard refresh exchange should succeed");

    const auto refresh_args = read_text(args_file);
    const auto refresh_body = read_text(deref_file);
    Expect(refresh_args.find(kRefreshToken) == std::string::npos,
        "Phase 1.1: ExecuteOAuthRefreshTokenExchange must NOT spill refresh_token onto curl argv");
    Expect(refresh_args.find("refresh_token=") == std::string::npos,
        "Phase 1.1: ExecuteOAuthRefreshTokenExchange must NOT pass refresh_token= as argv");
    Expect(refresh_args.find("@") != std::string::npos,
        "Phase 1.1: ExecuteOAuthRefreshTokenExchange must reference body via curl @file syntax");
    Expect(refresh_body.find(kRefreshToken) != std::string::npos,
        "Phase 1.1: refresh_token should still reach the request body via the deref'd file");
}

std::vector<agentos::AuthProviderDescriptor> SampleInteractiveProviders() {
    using agentos::AuthMode;
    using agentos::AuthProviderDescriptor;
    using agentos::AuthProviderId;
    return {
        AuthProviderDescriptor{
            .provider = AuthProviderId::openai,
            .provider_name = "openai",
            .supported_modes = {AuthMode::api_key, AuthMode::cli_session_passthrough},
            .browser_login_supported = false,
            .cli_session_passthrough_supported = true,
        },
        AuthProviderDescriptor{
            .provider = AuthProviderId::gemini,
            .provider_name = "gemini",
            .supported_modes = {AuthMode::api_key, AuthMode::browser_oauth, AuthMode::cli_session_passthrough},
            .browser_login_supported = true,
            .cli_session_passthrough_supported = true,
        },
        AuthProviderDescriptor{
            .provider = AuthProviderId::qwen,
            .provider_name = "qwen",
            .supported_modes = {AuthMode::api_key},
            .browser_login_supported = false,
        },
    };
}

agentos::OAuthProviderDefaults SampleInteractiveDefaults(const agentos::AuthProviderId provider) {
    return agentos::OAuthDefaultsForProvider(provider);
}

void TestInteractiveLoginPromptHappyPath() {
    // qwen + all defaults: empty input on every remaining prompt should
    // select api-key (the only mode), the AGENTOS_QWEN_API_KEY env hint,
    // profile=default, and set_default=true.  Provider is preselected so the
    // prompt sequence is: mode, api-key env, profile, set-default = 4 lines.
    std::stringstream input;
    input << "\n\n\n\n";
    std::stringstream out;
    const auto resolution = agentos::PromptInteractiveLogin(
        SampleInteractiveProviders(),
        SampleInteractiveDefaults,
        agentos::AuthProviderId::qwen,
        input,
        out);
    Expect(resolution.ok, "interactive prompt should succeed for preselected qwen + all defaults");
    Expect(resolution.provider == agentos::AuthProviderId::qwen,
        "interactive prompt should resolve qwen as the provider");
    Expect(resolution.mode == agentos::AuthMode::api_key,
        "interactive prompt should default qwen to api-key (the only supported mode)");
    Expect(resolution.api_key_env == "AGENTOS_QWEN_API_KEY",
        "interactive prompt should default api-key env to AGENTOS_<PROVIDER>_API_KEY");
    Expect(resolution.profile_name == "default",
        "interactive prompt should default profile name to 'default'");
    Expect(resolution.set_default,
        "interactive prompt should default set_default to true on empty answer");

    // The transcript should surface origin and note metadata for qwen, since
    // its OAuth defaults are stubbed and have a note describing why.
    const auto transcript = out.str();
    Expect(transcript.find("origin=stub") != std::string::npos,
        "interactive prompt should print origin metadata from OAuthProviderDefaults");
    Expect(transcript.find("note:") != std::string::npos,
        "interactive prompt should surface note metadata when present");
}

void TestInteractiveLoginCustomEnvAndProfile() {
    // Custom answers: provider is preselected so we only need to drive
    // mode, env, profile, set-default in that order.
    std::stringstream input;
    input << "\n"               // mode index (default 1 = api-key)
          << "MY_QWEN_KEY\n"    // api key env
          << "team\n"            // profile
          << "n\n";               // set as default? -> false
    std::stringstream out;
    const auto resolution = agentos::PromptInteractiveLogin(
        SampleInteractiveProviders(),
        SampleInteractiveDefaults,
        agentos::AuthProviderId::qwen,
        input,
        out);
    Expect(resolution.ok, "interactive prompt should accept custom env/profile/no-default answers");
    Expect(resolution.api_key_env == "MY_QWEN_KEY",
        "non-empty env answer should override the default");
    Expect(resolution.profile_name == "team",
        "non-empty profile answer should override the default");
    Expect(!resolution.set_default,
        "explicit 'n' to set-default should resolve to false");

    const auto argv = agentos::BuildLoginArgvFromResolution(resolution);
    Expect(argv.size() == 8,
        "argv should contain agentos auth login <provider> mode=... api_key_env=... profile=... set_default=...");
    bool found_env = false;
    bool found_profile = false;
    bool found_set_default = false;
    bool found_mode = false;
    for (const auto& argument : argv) {
        if (argument == "api_key_env=MY_QWEN_KEY") found_env = true;
        if (argument == "profile=team") found_profile = true;
        if (argument == "set_default=false") found_set_default = true;
        if (argument == "mode=api-key") found_mode = true;
    }
    Expect(found_env, "BuildLoginArgvFromResolution should emit api_key_env arg");
    Expect(found_profile, "BuildLoginArgvFromResolution should emit profile arg");
    Expect(found_set_default, "BuildLoginArgvFromResolution should emit set_default=false");
    Expect(found_mode, "BuildLoginArgvFromResolution should emit mode=api-key");
}

void TestInteractiveLoginEofIsClean() {
    // An empty stream simulates EOF before any answer can be read.  The
    // prompt should not loop forever and should report a clear error.
    std::stringstream empty_input;
    std::stringstream out;
    const auto resolution = agentos::PromptInteractiveLogin(
        SampleInteractiveProviders(),
        SampleInteractiveDefaults,
        std::nullopt,
        empty_input,
        out);
    Expect(!resolution.ok, "interactive prompt should fail on EOF before provider selection");
    Expect(resolution.error_message.find("stdin closed") != std::string::npos,
        "EOF error message should mention stdin");
}

void TestInteractiveLoginBrowserOAuthRedirects() {
    // Preselect gemini (which has browser_oauth defaults).  The mode menu
    // order is api-key (1), cli-session (2), browser_oauth (3).
    std::stringstream input;
    input << "3\n";  // mode index 3 = browser_oauth for gemini
    std::stringstream out;
    const auto resolution = agentos::PromptInteractiveLogin(
        SampleInteractiveProviders(),
        SampleInteractiveDefaults,
        agentos::AuthProviderId::gemini,
        input,
        out);
    Expect(!resolution.ok, "browser_oauth selection should not produce an ok resolution");
    Expect(resolution.redirect_to_oauth_login,
        "browser_oauth selection should set redirect_to_oauth_login");
    Expect(resolution.error_message.find("auth oauth-login") != std::string::npos,
        "browser_oauth redirect message should point to auth oauth-login");
}

void TestInteractiveLoginIndexedProviderChoice() {
    // "3\n" picks the third provider (qwen), then \n on every remaining
    // prompt (mode, env, profile, set-default) accepts defaults.
    std::stringstream input;
    input << "3\n\n\n\n\n";
    std::stringstream out;
    const auto resolution = agentos::PromptInteractiveLogin(
        SampleInteractiveProviders(),
        SampleInteractiveDefaults,
        std::nullopt,
        input,
        out);
    Expect(resolution.ok, "non-preselected provider choice should succeed");
    Expect(resolution.provider == agentos::AuthProviderId::qwen,
        "indexed choice 3 should resolve to qwen given the sample provider list");
}

void TestInteractiveLoginInvalidIndexFallsBackToFirst() {
    // "abc\n" is not numeric; the prompt should fall back to index 1 (openai)
    // and emit a hint, but otherwise continue.  Default mode (api-key) for
    // openai then triggers env/profile/set-default prompts.
    std::stringstream input;
    input << "abc\n\n\n\n\n";
    std::stringstream out;
    const auto resolution = agentos::PromptInteractiveLogin(
        SampleInteractiveProviders(),
        SampleInteractiveDefaults,
        std::nullopt,
        input,
        out);
    Expect(resolution.ok, "non-numeric answer should fall back to default and continue");
    Expect(resolution.provider == agentos::AuthProviderId::openai,
        "non-numeric provider answer should fall back to provider index 1 (openai)");
    Expect(out.str().find("invalid selection") != std::string::npos,
        "non-numeric answer should print a single-line hint");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestAuthApiKeySession(workspace);
    TestAuthLoginFlowModulesWithFixtureStores(workspace);
    TestSecureRandomBytesContract(workspace);
    TestOAuthListenerRequestValidation(workspace);
    TestOAuthPkceScaffold(workspace);
    TestOAuthExchangeDoesNotLeakSecretsOnArgv(workspace);
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
    TestInteractiveLoginPromptHappyPath();
    TestInteractiveLoginCustomEnvAndProfile();
    TestInteractiveLoginEofIsClean();
    TestInteractiveLoginBrowserOAuthRedirects();
    TestInteractiveLoginIndexedProviderChoice();
    TestInteractiveLoginInvalidIndexFallsBackToFirst();

    if (failures != 0) {
        std::cerr << failures << " auth test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_auth_tests passed\n";
    return 0;
}
