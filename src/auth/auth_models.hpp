#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

enum class AuthProviderId {
    openai,
    gemini,
    anthropic,
    qwen,
};

enum class AuthMode {
    browser_oauth,
    api_key,
    cli_session_passthrough,
    cloud_adc,
    cloud_bearer_token,
};

struct AuthSession {
    std::string session_id;
    AuthProviderId provider = AuthProviderId::openai;
    AuthMode mode = AuthMode::api_key;
    std::string profile_name = "default";
    std::string account_label;
    bool managed_by_agentos = false;
    bool managed_by_external_cli = false;
    bool refresh_supported = false;
    bool headless_compatible = true;
    std::string access_token_ref;
    std::string refresh_token_ref;
    std::chrono::system_clock::time_point expires_at = std::chrono::system_clock::time_point::max();
    std::map<std::string, std::string> metadata;
};

struct AuthStatus {
    bool authenticated = false;
    bool expired = false;
    bool refreshable = false;
    std::string provider_name;
    std::string profile_name = "default";
    std::string account_label;
    std::string mode;
    std::string message;
    bool managed_by_agentos = false;
    bool managed_by_external_cli = false;
};

struct AuthProviderDescriptor {
    AuthProviderId provider = AuthProviderId::openai;
    std::string provider_name;
    std::vector<AuthMode> supported_modes;
    bool browser_login_supported = false;
    bool headless_supported = true;
    bool refresh_token_supported = false;
    bool cli_session_passthrough_supported = false;
};

std::string ToString(AuthProviderId provider);
std::optional<AuthProviderId> ParseAuthProviderId(const std::string& value);

std::string ToString(AuthMode mode);
std::optional<AuthMode> ParseAuthMode(const std::string& value);

std::string MakeAuthSessionId(AuthProviderId provider, AuthMode mode, const std::string& profile_name);
bool IsAuthSessionExpired(const AuthSession& session);

}  // namespace agentos

