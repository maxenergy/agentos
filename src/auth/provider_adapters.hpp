#pragma once

#include "auth/auth_provider_adapter.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/cli/cli_host.hpp"

#include <filesystem>
#include <string>

namespace agentos {

class StaticAuthProviderAdapter : public IAuthProviderAdapter {
public:
    StaticAuthProviderAdapter(
        AuthProviderDescriptor descriptor,
        SessionStore& session_store,
        SecureTokenStore& token_store,
        const CliHost* cli_host,
        std::filesystem::path workspace_path);

    AuthProviderDescriptor descriptor() const override;
    std::vector<AuthMode> supported_modes() const override;
    AuthSession login(AuthMode mode, const std::map<std::string, std::string>& options) override;
    AuthStatus status(const std::string& profile_name) override;
    AuthSession refresh(const AuthSession& session) override;
    void logout(const std::string& profile_name) override;
    std::optional<AuthSession> probe_external_session() override;

protected:
    virtual std::optional<AuthSession> probe_cli_session();
    virtual std::string default_api_key_env() const;
    AuthSession make_api_key_session(const std::string& profile_name, const std::string& env_name) const;
    AuthStatus status_from_session(const AuthSession& session, const std::string& message) const;

    AuthProviderDescriptor descriptor_;
    SessionStore& session_store_;
    SecureTokenStore& token_store_;
    const CliHost* cli_host_ = nullptr;
    std::filesystem::path workspace_path_;
};

class OpenAiAuthProviderAdapter final : public StaticAuthProviderAdapter {
public:
    OpenAiAuthProviderAdapter(SessionStore& session_store, SecureTokenStore& token_store, const CliHost& cli_host, std::filesystem::path workspace_path);

protected:
    std::optional<AuthSession> probe_cli_session() override;
    std::string default_api_key_env() const override;
};

class AnthropicAuthProviderAdapter final : public StaticAuthProviderAdapter {
public:
    AnthropicAuthProviderAdapter(SessionStore& session_store, SecureTokenStore& token_store, const CliHost& cli_host, std::filesystem::path workspace_path);

protected:
    std::optional<AuthSession> probe_cli_session() override;
    std::string default_api_key_env() const override;
};

class GeminiAuthProviderAdapter final : public StaticAuthProviderAdapter {
public:
    GeminiAuthProviderAdapter(SessionStore& session_store, SecureTokenStore& token_store);

protected:
    std::string default_api_key_env() const override;
};

class QwenAuthProviderAdapter final : public StaticAuthProviderAdapter {
public:
    QwenAuthProviderAdapter(SessionStore& session_store, SecureTokenStore& token_store);

protected:
    std::string default_api_key_env() const override;
};

}  // namespace agentos

