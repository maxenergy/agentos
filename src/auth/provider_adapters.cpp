#include "auth/provider_adapters.hpp"

#include "auth/oauth_pkce.hpp"
#include "utils/command_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

bool ClaudeAuthStatusReportsLoggedIn(const std::string& stdout_text) {
    // Claude CLI's `auth status` emits a JSON object with a `loggedIn` boolean.
    // Previously we did a substring match on the literal text
    // `"loggedIn": true`, which broke on whitespace, key ordering, and any
    // re-encoding by the CLI. Parse the response properly and inspect the
    // field; on any parse error preserve the legacy semantics by returning
    // false (i.e. treat malformed output as "not logged in").
    try {
        const auto parsed = nlohmann::json::parse(stdout_text);
        if (!parsed.is_object()) {
            return false;
        }
        const auto it = parsed.find("loggedIn");
        if (it == parsed.end() || !it->is_boolean()) {
            return false;
        }
        return it->get<bool>();
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

bool SupportsMode(const std::vector<AuthMode>& modes, const AuthMode mode) {
    return std::find(modes.begin(), modes.end(), mode) != modes.end();
}

std::string OptionOrDefault(
    const std::map<std::string, std::string>& options,
    const std::string& key,
    const std::string& default_value) {
    const auto it = options.find(key);
    if (it == options.end() || it->second.empty()) {
        return default_value;
    }
    return it->second;
}

std::chrono::system_clock::time_point LongLivedSessionExpiry() {
    return std::chrono::system_clock::now() + std::chrono::hours(24 * 365);
}

bool HasNativeOAuthCompletionOptions(const std::map<std::string, std::string>& options) {
    return options.contains("callback_url") &&
           options.contains("state") &&
           options.contains("code_verifier") &&
           options.contains("redirect_uri") &&
           options.contains("client_id");
}

AuthStatus MissingSessionStatus(const AuthProviderDescriptor& descriptor, const std::string& profile_name) {
    return {
        .authenticated = false,
        .provider_name = descriptor.provider_name,
        .profile_name = profile_name,
        .message = "no session found",
    };
}

AuthSession MakeCliSession(
    const AuthProviderId provider,
    const std::string& provider_name,
    const std::string& profile_name,
    const std::string& account_label,
    const std::string& cli_binary,
    const std::string& raw_status) {
    return {
        .session_id = MakeAuthSessionId(provider, AuthMode::cli_session_passthrough, profile_name),
        .provider = provider,
        .mode = AuthMode::cli_session_passthrough,
        .profile_name = profile_name,
        .account_label = account_label,
        .managed_by_agentos = false,
        .managed_by_external_cli = true,
        .refresh_supported = false,
        .headless_compatible = false,
        .access_token_ref = "external-cli:" + cli_binary,
        .expires_at = LongLivedSessionExpiry(),
        .metadata = {
            {"provider", provider_name},
            {"cli", cli_binary},
            {"probe", raw_status},
        },
    };
}

std::optional<std::filesystem::path> HomeDirectory() {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, "USERPROFILE") != 0 || raw_value == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path value(raw_value);
    std::free(raw_value);
    return value;
#else
    const char* raw_value = std::getenv("HOME");
    if (!raw_value) {
        return std::nullopt;
    }
    return std::filesystem::path(raw_value);
#endif
}

bool CodexAuthFileLooksAvailable() {
    const auto home = HomeDirectory();
    if (!home.has_value()) {
        return false;
    }

    const auto auth_file = *home / ".codex" / "auth.json";
    std::ifstream input(auth_file, std::ios::binary);
    if (!input) {
        return false;
    }

    std::string first_chunk(512, '\0');
    input.read(first_chunk.data(), static_cast<std::streamsize>(first_chunk.size()));
    first_chunk.resize(static_cast<std::size_t>(input.gcount()));

    return first_chunk.find("\"auth_mode\"") != std::string::npos;
}

bool GeminiOAuthFileLooksAvailable() {
    const auto home = HomeDirectory();
    if (!home.has_value()) {
        return false;
    }

    const auto auth_file = *home / ".gemini" / "oauth_creds.json";
    std::ifstream input(auth_file, std::ios::binary);
    if (!input) {
        return false;
    }

    std::string first_chunk(4096, '\0');
    input.read(first_chunk.data(), static_cast<std::streamsize>(first_chunk.size()));
    first_chunk.resize(static_cast<std::size_t>(input.gcount()));

    return first_chunk.find("access_token") != std::string::npos ||
           first_chunk.find("refresh_token") != std::string::npos;
}

bool GoogleAdcLooksAvailable() {
#ifdef _WIN32
    char* raw_credentials_file = nullptr;
    std::size_t credentials_file_size = 0;
    if (_dupenv_s(&raw_credentials_file, &credentials_file_size, "GOOGLE_APPLICATION_CREDENTIALS") == 0 &&
        raw_credentials_file != nullptr) {
        const std::filesystem::path credentials_file(raw_credentials_file);
        std::free(raw_credentials_file);
        if (std::filesystem::exists(credentials_file)) {
            return true;
        }
    }
#else
    const char* credentials_file = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (credentials_file && std::filesystem::exists(credentials_file)) {
        return true;
    }
#endif

    const auto home = HomeDirectory();
    if (home.has_value()) {
        const auto posix_adc_file = *home / ".config" / "gcloud" / "application_default_credentials.json";
        if (std::filesystem::exists(posix_adc_file)) {
            return true;
        }
    }

#ifdef _WIN32
    char* raw_appdata = nullptr;
    std::size_t appdata_size = 0;
    if (_dupenv_s(&raw_appdata, &appdata_size, "APPDATA") == 0 && raw_appdata != nullptr) {
        const std::filesystem::path appdata(raw_appdata);
        std::free(raw_appdata);
        const auto windows_adc_file = appdata / "gcloud" / "application_default_credentials.json";
        if (std::filesystem::exists(windows_adc_file)) {
            return true;
        }
    }
#endif

    return false;
}

AuthSession MakeCloudAdcSession(
    const AuthProviderId provider,
    const std::string& provider_name,
    const std::string& profile_name) {
    return {
        .session_id = MakeAuthSessionId(provider, AuthMode::cloud_adc, profile_name),
        .provider = provider,
        .mode = AuthMode::cloud_adc,
        .profile_name = profile_name,
        .account_label = "google-application-default-credentials",
        .managed_by_agentos = false,
        .managed_by_external_cli = true,
        .refresh_supported = true,
        .headless_compatible = true,
        .access_token_ref = "external-cli:gcloud-adc",
        .expires_at = LongLivedSessionExpiry(),
        .metadata = {
            {"provider", provider_name},
            {"credential_source", "google_adc"},
            {"cli", "gcloud"},
        },
    };
}

}  // namespace

StaticAuthProviderAdapter::StaticAuthProviderAdapter(
    AuthProviderDescriptor descriptor,
    SessionStore& session_store,
    SecureTokenStore& token_store,
    const CliHost* cli_host,
    std::filesystem::path workspace_path)
    : descriptor_(std::move(descriptor)),
      session_store_(session_store),
      token_store_(token_store),
      cli_host_(cli_host),
      workspace_path_(std::move(workspace_path)) {}

AuthProviderDescriptor StaticAuthProviderAdapter::descriptor() const {
    return descriptor_;
}

std::vector<AuthMode> StaticAuthProviderAdapter::supported_modes() const {
    return descriptor_.supported_modes;
}

AuthSession StaticAuthProviderAdapter::login(const AuthMode mode, const std::map<std::string, std::string>& options) {
    if (!SupportsMode(descriptor_.supported_modes, mode)) {
        throw std::runtime_error("UnsupportedAuthMode");
    }

    const auto profile_name = OptionOrDefault(options, "profile", "default");
    AuthSession session;

    if (mode == AuthMode::api_key) {
        const auto env_name = OptionOrDefault(options, "api_key_env", default_api_key_env());
        session = make_api_key_session(profile_name, env_name);
    } else if (mode == AuthMode::cli_session_passthrough) {
        auto probed_session = probe_cli_session();
        if (!probed_session.has_value()) {
            throw std::runtime_error("CliSessionUnavailable");
        }
        probed_session->profile_name = profile_name;
        probed_session->session_id = MakeAuthSessionId(descriptor_.provider, AuthMode::cli_session_passthrough, profile_name);
        session = *probed_session;
    } else if (mode == AuthMode::browser_oauth) {
        if (HasNativeOAuthCompletionOptions(options)) {
            if (!cli_host_) {
                throw std::runtime_error("NativeOAuthUnavailable");
            }
            const auto callback = ValidateOAuthCallbackUrl(OAuthPkceStart{
                .provider = descriptor_.provider,
                .profile_name = profile_name,
                .state = options.at("state"),
                .code_verifier = options.at("code_verifier"),
                .redirect_uri = options.at("redirect_uri"),
            }, options.at("callback_url"));
            const auto defaults = OAuthDefaultsForProvider(descriptor_.provider);
            const auto token_endpoint = OptionOrDefault(options, "token_endpoint", defaults.token_endpoint);
            session = CompleteOAuthLogin(
                *cli_host_,
                session_store_,
                token_store_,
                OAuthLoginOrchestrationInput{
                    .start = OAuthPkceStart{
                        .provider = descriptor_.provider,
                        .profile_name = profile_name,
                        .state = options.at("state"),
                        .code_verifier = options.at("code_verifier"),
                        .redirect_uri = options.at("redirect_uri"),
                    },
                    .callback = callback,
                    .token_endpoint = token_endpoint,
                    .client_id = options.at("client_id"),
                    .account_label = OptionOrDefault(options, "account_label", descriptor_.provider_name + ":" + profile_name),
                },
                workspace_path_);
        } else {
            auto probed_session = probe_cli_session();
            if (!probed_session.has_value()) {
                throw std::runtime_error("BrowserOAuthUnavailable");
            }
            probed_session->mode = AuthMode::browser_oauth;
            probed_session->profile_name = profile_name;
            probed_session->session_id = MakeAuthSessionId(descriptor_.provider, AuthMode::browser_oauth, profile_name);
            session = *probed_session;
        }
    } else if (mode == AuthMode::cloud_adc) {
        if (descriptor_.provider != AuthProviderId::gemini) {
            throw std::runtime_error("UnsupportedAuthMode");
        }
        if ((!CommandExists("gcloud") && !CommandExists("gcloud.cmd")) || !GoogleAdcLooksAvailable()) {
            throw std::runtime_error("CloudAdcUnavailable");
        }
        session = MakeCloudAdcSession(descriptor_.provider, descriptor_.provider_name, profile_name);
    } else {
        throw std::runtime_error("UnsupportedAuthMode");
    }

    session_store_.save(session);
    return session;
}

AuthStatus StaticAuthProviderAdapter::status(const std::string& profile_name) {
    const auto session = session_store_.find(descriptor_.provider, profile_name);
    if (!session.has_value()) {
        if (const auto probed = probe_cli_session(); probed.has_value()) {
            return status_from_session(*probed, "external CLI session detected but not imported");
        }
        return MissingSessionStatus(descriptor_, profile_name);
    }

    return status_from_session(*session, IsAuthSessionExpired(*session) ? "session expired" : "session available");
}

AuthSession StaticAuthProviderAdapter::refresh(const AuthSession& session) {
    if (!session.refresh_supported) {
        throw std::runtime_error("RefreshUnsupported");
    }
    if (session.metadata.contains("credential_source") &&
        session.metadata.at("credential_source") == "oauth_pkce" &&
        session.metadata.contains("token_endpoint") &&
        session.metadata.contains("client_id") &&
        cli_host_) {
        return RefreshOAuthSession(
            *cli_host_,
            session_store_,
            token_store_,
            OAuthRefreshOrchestrationInput{
                .existing_session = session,
                .token_endpoint = session.metadata.at("token_endpoint"),
                .client_id = session.metadata.at("client_id"),
            },
            workspace_path_);
    }

    auto refreshed = session;
    refreshed.expires_at = LongLivedSessionExpiry();
    refreshed.metadata["refreshed_by"] = "static-adapter";
    return refreshed;
}

void StaticAuthProviderAdapter::logout(const std::string& profile_name) {
    session_store_.remove(descriptor_.provider, profile_name);
}

std::optional<AuthSession> StaticAuthProviderAdapter::probe_external_session() {
    return probe_cli_session();
}

std::optional<AuthSession> StaticAuthProviderAdapter::probe_cli_session() {
    return std::nullopt;
}

std::string StaticAuthProviderAdapter::default_api_key_env() const {
    return ToString(descriptor_.provider) + "_API_KEY";
}

AuthSession StaticAuthProviderAdapter::make_api_key_session(const std::string& profile_name, const std::string& env_name) const {
    const auto token_ref = token_store_.make_env_ref(env_name);
    if (!token_store_.ref_available(token_ref)) {
        throw std::runtime_error("InvalidCredential: environment variable is missing");
    }

    return {
        .session_id = MakeAuthSessionId(descriptor_.provider, AuthMode::api_key, profile_name),
        .provider = descriptor_.provider,
        .mode = AuthMode::api_key,
        .profile_name = profile_name,
        .account_label = env_name,
        .managed_by_agentos = true,
        .managed_by_external_cli = false,
        .refresh_supported = false,
        .headless_compatible = true,
        .access_token_ref = token_ref,
        .expires_at = std::chrono::system_clock::time_point::max(),
        .metadata = {
            {"credential_source", "environment"},
        },
    };
}

AuthStatus StaticAuthProviderAdapter::status_from_session(const AuthSession& session, const std::string& message) const {
    const auto expired = IsAuthSessionExpired(session);
    const auto token_available = session.managed_by_external_cli || token_store_.ref_available(session.access_token_ref);
    return {
        .authenticated = !expired && token_available,
        .expired = expired,
        .refreshable = session.refresh_supported,
        .provider_name = descriptor_.provider_name,
        .profile_name = session.profile_name,
        .account_label = session.account_label,
        .mode = ToString(session.mode),
        .message = token_available ? message : "credential reference is unavailable",
        .managed_by_agentos = session.managed_by_agentos,
        .managed_by_external_cli = session.managed_by_external_cli,
    };
}

OpenAiAuthProviderAdapter::OpenAiAuthProviderAdapter(
    SessionStore& session_store,
    SecureTokenStore& token_store,
    const CliHost& cli_host,
    std::filesystem::path workspace_path)
    : StaticAuthProviderAdapter(
          AuthProviderDescriptor{
              .provider = AuthProviderId::openai,
              .provider_name = "openai",
              .supported_modes = {AuthMode::api_key, AuthMode::cli_session_passthrough, AuthMode::browser_oauth},
              .browser_login_supported = true,
              .headless_supported = true,
              .refresh_token_supported = true,
              .cli_session_passthrough_supported = true,
          },
          session_store,
          token_store,
          &cli_host,
          std::move(workspace_path)) {}

std::optional<AuthSession> OpenAiAuthProviderAdapter::probe_cli_session() {
    if (!cli_host_ || !CommandExists("codex")) {
        return std::nullopt;
    }

    const auto result = cli_host_->run(CliRunRequest{
        .spec = CliSpec{
            .name = "codex_login_status_probe",
            .description = "Probe Codex CLI login status.",
            .binary = "codex",
            .args_template = {"login", "status"},
            .parse_mode = "text",
            .risk_level = "low",
            .permissions = {"process.spawn"},
            .timeout_ms = 5000,
            .env_allowlist = {"USERPROFILE", "HOMEDRIVE", "HOMEPATH", "HOME", "APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME", "CODEX_HOME"},
        },
        .workspace_path = workspace_path_,
    });

    if (!result.success || result.stdout_text.find("Logged in") == std::string::npos) {
        if (CodexAuthFileLooksAvailable()) {
            return MakeCliSession(AuthProviderId::openai, "openai", "default", "codex-cli", "codex", "auth-file-present");
        }
        return std::nullopt;
    }

    return MakeCliSession(AuthProviderId::openai, "openai", "default", "codex-cli", "codex", result.stdout_text);
}

std::string OpenAiAuthProviderAdapter::default_api_key_env() const {
    return "OPENAI_API_KEY";
}

AnthropicAuthProviderAdapter::AnthropicAuthProviderAdapter(
    SessionStore& session_store,
    SecureTokenStore& token_store,
    const CliHost& cli_host,
    std::filesystem::path workspace_path)
    : StaticAuthProviderAdapter(
          AuthProviderDescriptor{
              .provider = AuthProviderId::anthropic,
              .provider_name = "anthropic",
              .supported_modes = {AuthMode::api_key, AuthMode::cli_session_passthrough, AuthMode::browser_oauth},
              .browser_login_supported = true,
              .headless_supported = true,
              .refresh_token_supported = true,
              .cli_session_passthrough_supported = true,
          },
          session_store,
          token_store,
          &cli_host,
          std::move(workspace_path)) {}

std::optional<AuthSession> AnthropicAuthProviderAdapter::probe_cli_session() {
    if (!cli_host_ || !CommandExists("claude")) {
        return std::nullopt;
    }

    const auto result = cli_host_->run(CliRunRequest{
        .spec = CliSpec{
            .name = "claude_auth_status_probe",
            .description = "Probe Claude CLI authentication status.",
            .binary = "claude",
            .args_template = {"auth", "status"},
            .parse_mode = "json",
            .risk_level = "low",
            .permissions = {"process.spawn"},
            .timeout_ms = 5000,
            .env_allowlist = {"USERPROFILE", "HOMEDRIVE", "HOMEPATH", "HOME", "APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME", "CLAUDE_CONFIG_DIR"},
        },
        .workspace_path = workspace_path_,
    });

    if (!result.success || !ClaudeAuthStatusReportsLoggedIn(result.stdout_text)) {
        return std::nullopt;
    }

    return MakeCliSession(AuthProviderId::anthropic, "anthropic", "default", "claude-cli", "claude", "loggedIn=true");
}

std::string AnthropicAuthProviderAdapter::default_api_key_env() const {
    return "ANTHROPIC_API_KEY";
}

GeminiAuthProviderAdapter::GeminiAuthProviderAdapter(SessionStore& session_store, SecureTokenStore& token_store)
    : StaticAuthProviderAdapter(
          AuthProviderDescriptor{
              .provider = AuthProviderId::gemini,
              .provider_name = "gemini",
              .supported_modes = {AuthMode::api_key, AuthMode::browser_oauth, AuthMode::cli_session_passthrough, AuthMode::cloud_adc},
              .browser_login_supported = true,
              .headless_supported = true,
              .refresh_token_supported = true,
              .cli_session_passthrough_supported = true,
          },
          session_store,
          token_store,
          nullptr,
          {}) {}

GeminiAuthProviderAdapter::GeminiAuthProviderAdapter(
    SessionStore& session_store,
    SecureTokenStore& token_store,
    const CliHost& cli_host,
    std::filesystem::path workspace_path)
    : StaticAuthProviderAdapter(
          AuthProviderDescriptor{
              .provider = AuthProviderId::gemini,
              .provider_name = "gemini",
              .supported_modes = {AuthMode::api_key, AuthMode::browser_oauth, AuthMode::cli_session_passthrough, AuthMode::cloud_adc},
              .browser_login_supported = true,
              .headless_supported = true,
              .refresh_token_supported = true,
              .cli_session_passthrough_supported = true,
          },
          session_store,
          token_store,
          &cli_host,
          std::move(workspace_path)) {}

std::optional<AuthSession> GeminiAuthProviderAdapter::probe_cli_session() {
    if (!CommandExists("gemini") && !CommandExists("gemini.cmd")) {
        return std::nullopt;
    }

    if (GeminiOAuthFileLooksAvailable()) {
        return MakeCliSession(AuthProviderId::gemini, "gemini", "default", "gemini-cli-oauth", "gemini", "oauth-file-present");
    }

    return std::nullopt;
}

std::string GeminiAuthProviderAdapter::default_api_key_env() const {
    return "GEMINI_API_KEY";
}

QwenAuthProviderAdapter::QwenAuthProviderAdapter(SessionStore& session_store, SecureTokenStore& token_store)
    : StaticAuthProviderAdapter(
          AuthProviderDescriptor{
              .provider = AuthProviderId::qwen,
              .provider_name = "qwen",
              .supported_modes = {AuthMode::api_key},
              .headless_supported = true,
          },
          session_store,
          token_store,
          nullptr,
          {}) {}

std::string QwenAuthProviderAdapter::default_api_key_env() const {
    return "QWEN_API_KEY";
}

}  // namespace agentos
