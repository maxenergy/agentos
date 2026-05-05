#pragma once

#include "auth/auth_models.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/cli/cli_host.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace agentos {

using AuthCliSessionProbe = std::function<std::optional<AuthSession>()>;
using AuthAvailabilityProbe = std::function<bool()>;

struct AuthLoginFlowContext {
    AuthProviderDescriptor descriptor;
    SessionStore& session_store;
    SecureTokenStore& token_store;
    const CliHost* cli_host = nullptr;
    std::filesystem::path workspace_path;
    std::string default_api_key_env;
    AuthCliSessionProbe probe_cli_session;
    AuthAvailabilityProbe cloud_adc_available;
};

AuthSession LoginWithApiKeyEnvRef(
    const AuthLoginFlowContext& context,
    const std::string& profile_name,
    const std::map<std::string, std::string>& options);

AuthSession LoginWithCliSessionPassthrough(
    const AuthLoginFlowContext& context,
    const std::string& profile_name);

AuthSession LoginWithBrowserOAuthPkce(
    const AuthLoginFlowContext& context,
    const std::string& profile_name,
    const std::map<std::string, std::string>& options);

AuthSession LoginWithCloudAdc(
    const AuthLoginFlowContext& context,
    const std::string& profile_name);

AuthSession RefreshAuthLoginFlow(
    const AuthLoginFlowContext& context,
    const AuthSession& session);

}  // namespace agentos
