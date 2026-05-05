#include "auth/auth_login_flow.hpp"

#include "auth/oauth_pkce.hpp"

#include <chrono>
#include <stdexcept>

namespace agentos {
namespace {

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

}  // namespace

AuthSession LoginWithApiKeyEnvRef(
    const AuthLoginFlowContext& context,
    const std::string& profile_name,
    const std::map<std::string, std::string>& options) {
    const auto env_name = OptionOrDefault(options, "api_key_env", context.default_api_key_env);
    const auto token_ref = context.token_store.make_env_ref(env_name);
    if (!context.token_store.ref_available(token_ref)) {
        throw std::runtime_error("InvalidCredential: environment variable is missing");
    }

    return {
        .session_id = MakeAuthSessionId(context.descriptor.provider, AuthMode::api_key, profile_name),
        .provider = context.descriptor.provider,
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

AuthSession LoginWithCliSessionPassthrough(
    const AuthLoginFlowContext& context,
    const std::string& profile_name) {
    if (!context.probe_cli_session) {
        throw std::runtime_error("CliSessionUnavailable");
    }
    auto probed_session = context.probe_cli_session();
    if (!probed_session.has_value()) {
        throw std::runtime_error("CliSessionUnavailable");
    }
    probed_session->profile_name = profile_name;
    probed_session->session_id = MakeAuthSessionId(
        context.descriptor.provider,
        AuthMode::cli_session_passthrough,
        profile_name);
    return *probed_session;
}

AuthSession LoginWithBrowserOAuthPkce(
    const AuthLoginFlowContext& context,
    const std::string& profile_name,
    const std::map<std::string, std::string>& options) {
    if (HasNativeOAuthCompletionOptions(options)) {
        if (!context.cli_host) {
            throw std::runtime_error("NativeOAuthUnavailable");
        }
        const auto callback = ValidateOAuthCallbackUrl(OAuthPkceStart{
            .provider = context.descriptor.provider,
            .profile_name = profile_name,
            .state = options.at("state"),
            .code_verifier = options.at("code_verifier"),
            .redirect_uri = options.at("redirect_uri"),
        }, options.at("callback_url"));
        const auto defaults = OAuthDefaultsForProvider(context.descriptor.provider);
        const auto token_endpoint = OptionOrDefault(options, "token_endpoint", defaults.token_endpoint);
        return CompleteOAuthLogin(
            *context.cli_host,
            context.session_store,
            context.token_store,
            OAuthLoginOrchestrationInput{
                .start = OAuthPkceStart{
                    .provider = context.descriptor.provider,
                    .profile_name = profile_name,
                    .state = options.at("state"),
                    .code_verifier = options.at("code_verifier"),
                    .redirect_uri = options.at("redirect_uri"),
                },
                .callback = callback,
                .token_endpoint = token_endpoint,
                .client_id = options.at("client_id"),
                .account_label = OptionOrDefault(
                    options,
                    "account_label",
                    context.descriptor.provider_name + ":" + profile_name),
            },
            context.workspace_path);
    }

    if (!context.probe_cli_session) {
        throw std::runtime_error("BrowserOAuthUnavailable");
    }
    auto probed_session = context.probe_cli_session();
    if (!probed_session.has_value()) {
        throw std::runtime_error("BrowserOAuthUnavailable");
    }
    probed_session->mode = AuthMode::browser_oauth;
    probed_session->profile_name = profile_name;
    probed_session->session_id = MakeAuthSessionId(context.descriptor.provider, AuthMode::browser_oauth, profile_name);
    return *probed_session;
}

AuthSession LoginWithCloudAdc(
    const AuthLoginFlowContext& context,
    const std::string& profile_name) {
    if (context.descriptor.provider != AuthProviderId::gemini) {
        throw std::runtime_error("UnsupportedAuthMode");
    }
    if (!context.cloud_adc_available || !context.cloud_adc_available()) {
        throw std::runtime_error("CloudAdcUnavailable");
    }
    return {
        .session_id = MakeAuthSessionId(context.descriptor.provider, AuthMode::cloud_adc, profile_name),
        .provider = context.descriptor.provider,
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
            {"provider", context.descriptor.provider_name},
            {"credential_source", "google_adc"},
            {"cli", "gcloud"},
        },
    };
}

AuthSession RefreshAuthLoginFlow(
    const AuthLoginFlowContext& context,
    const AuthSession& session) {
    if (!session.refresh_supported) {
        throw std::runtime_error("RefreshUnsupported");
    }
    if (session.metadata.contains("credential_source") &&
        session.metadata.at("credential_source") == "oauth_pkce" &&
        session.metadata.contains("token_endpoint") &&
        session.metadata.contains("client_id") &&
        context.cli_host) {
        return RefreshOAuthSession(
            *context.cli_host,
            context.session_store,
            context.token_store,
            OAuthRefreshOrchestrationInput{
                .existing_session = session,
                .token_endpoint = session.metadata.at("token_endpoint"),
                .client_id = session.metadata.at("client_id"),
            },
            context.workspace_path);
    }

    auto refreshed = session;
    refreshed.expires_at = LongLivedSessionExpiry();
    refreshed.metadata["refreshed_by"] = "static-adapter";
    return refreshed;
}

}  // namespace agentos
