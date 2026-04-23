#include "auth/auth_models.hpp"

#include <algorithm>
#include <cctype>

namespace agentos {

namespace {

std::string Normalize(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        if (ch == '-' || ch == '_') {
            return '_';
        }
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

std::string ToString(const AuthProviderId provider) {
    switch (provider) {
    case AuthProviderId::openai:
        return "openai";
    case AuthProviderId::gemini:
        return "gemini";
    case AuthProviderId::anthropic:
        return "anthropic";
    case AuthProviderId::qwen:
        return "qwen";
    default:
        return "unknown";
    }
}

std::optional<AuthProviderId> ParseAuthProviderId(const std::string& value) {
    const auto normalized = Normalize(value);
    if (normalized == "openai" || normalized == "codex") {
        return AuthProviderId::openai;
    }
    if (normalized == "gemini" || normalized == "google") {
        return AuthProviderId::gemini;
    }
    if (normalized == "anthropic" || normalized == "claude") {
        return AuthProviderId::anthropic;
    }
    if (normalized == "qwen") {
        return AuthProviderId::qwen;
    }
    return std::nullopt;
}

std::string ToString(const AuthMode mode) {
    switch (mode) {
    case AuthMode::browser_oauth:
        return "browser_oauth";
    case AuthMode::api_key:
        return "api_key";
    case AuthMode::cli_session_passthrough:
        return "cli_session";
    case AuthMode::cloud_adc:
        return "cloud_adc";
    case AuthMode::cloud_bearer_token:
        return "cloud_bearer_token";
    default:
        return "unknown";
    }
}

std::optional<AuthMode> ParseAuthMode(const std::string& value) {
    const auto normalized = Normalize(value);
    if (normalized == "browser_oauth" || normalized == "oauth") {
        return AuthMode::browser_oauth;
    }
    if (normalized == "api_key" || normalized == "apikey") {
        return AuthMode::api_key;
    }
    if (normalized == "cli_session" || normalized == "cli_session_passthrough" || normalized == "cli") {
        return AuthMode::cli_session_passthrough;
    }
    if (normalized == "cloud_adc" || normalized == "adc") {
        return AuthMode::cloud_adc;
    }
    if (normalized == "cloud_bearer_token") {
        return AuthMode::cloud_bearer_token;
    }
    return std::nullopt;
}

std::string MakeAuthSessionId(const AuthProviderId provider, const AuthMode mode, const std::string& profile_name) {
    return ToString(provider) + ":" + profile_name + ":" + ToString(mode);
}

bool IsAuthSessionExpired(const AuthSession& session) {
    return session.expires_at != std::chrono::system_clock::time_point::max() &&
           session.expires_at <= std::chrono::system_clock::now();
}

}  // namespace agentos

