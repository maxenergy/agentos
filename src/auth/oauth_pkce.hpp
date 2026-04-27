#pragma once

#include "auth/auth_models.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/cli/cli_host.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

struct OAuthPkceStartRequest {
    AuthProviderId provider = AuthProviderId::openai;
    std::string profile_name = "default";
    std::string authorization_endpoint;
    std::string client_id;
    std::string redirect_uri;
    std::vector<std::string> scopes;
    std::string state;
    std::string code_verifier;
};

struct OAuthProviderDefaults {
    bool supported = false;
    std::string authorization_endpoint;
    std::string token_endpoint;
    std::vector<std::string> scopes;
    // Origin of the supported state: "builtin" when the values are baked into
    // the binary, "config" when overridden from the workspace TSV, "stub" when
    // we know the provider but have no PKCE-eligible defaults yet, or empty
    // when nothing is known.  Filled by OAuthDefaultsForProvider /
    // EffectiveOAuthDefaultsForProvider; merge sites preserve it sensibly.
    std::string origin;
    // Optional human-readable note explaining why the defaults are stubbed or
    // require manual configuration (e.g. "anthropic OAuth flow not yet
    // documented for end users — supply endpoints manually").
    std::string note;
};

struct OAuthPkceStart {
    AuthProviderId provider = AuthProviderId::openai;
    std::string profile_name = "default";
    std::string state;
    std::string code_verifier;
    std::string code_challenge;
    std::string code_challenge_method = "S256";
    std::string redirect_uri;
    std::string authorization_url;
};

struct OAuthCallbackResult {
    bool success = false;
    std::string code;
    std::string error;
    std::string error_description;
};

struct OAuthTokenRequest {
    std::string token_endpoint;
    std::string content_type = "application/x-www-form-urlencoded";
    std::string body;
};

struct OAuthTokenRequestInput {
    std::string token_endpoint;
    std::string client_id;
    std::string redirect_uri;
    std::string code;
    std::string code_verifier;
};

struct OAuthRefreshTokenRequestInput {
    std::string token_endpoint;
    std::string client_id;
    std::string refresh_token;
};

struct OAuthTokenResponse {
    bool success = false;
    std::string access_token;
    std::string refresh_token;
    std::string token_type;
    int expires_in_seconds = 0;
    std::string error;
    std::string error_description;
};

struct OAuthSessionPersistInput {
    AuthProviderId provider = AuthProviderId::openai;
    AuthMode mode = AuthMode::browser_oauth;
    std::string profile_name = "default";
    std::string account_label;
    std::string token_endpoint;
    std::string client_id;
    OAuthTokenResponse token_response;
};

struct OAuthCallbackListenRequest {
    OAuthPkceStart start;
    int port = 0;
    int timeout_ms = 120000;
};

struct OAuthLoginOrchestrationInput {
    OAuthPkceStart start;
    OAuthCallbackResult callback;
    std::string token_endpoint;
    std::string client_id;
    std::string account_label;
};

struct OAuthRefreshOrchestrationInput {
    AuthSession existing_session;
    std::string token_endpoint;
    std::string client_id;
};

OAuthProviderDefaults OAuthDefaultsForProvider(AuthProviderId provider);
OAuthProviderDefaults MergeOAuthProviderDefaults(
    const OAuthProviderDefaults& base,
    const OAuthProviderDefaults& override_defaults);
std::map<AuthProviderId, OAuthProviderDefaults> LoadOAuthProviderDefaultsFromFile(
    const std::filesystem::path& path);
std::optional<OAuthProviderDefaults> LoadOAuthProviderDefaultsFromFile(
    const std::filesystem::path& path,
    AuthProviderId provider);
OAuthPkceStart CreateOAuthPkceStart(OAuthPkceStartRequest request);
OAuthCallbackResult ValidateOAuthCallback(
    const OAuthPkceStart& start,
    const std::string& returned_state,
    const std::string& code,
    const std::string& error = {},
    const std::string& error_description = {});
OAuthCallbackResult ValidateOAuthCallbackUrl(
    const OAuthPkceStart& start,
    const std::string& callback_url);
OAuthCallbackResult ListenForOAuthCallbackOnce(const OAuthCallbackListenRequest& request);
OAuthTokenRequest BuildOAuthTokenRequest(const OAuthTokenRequestInput& input);
OAuthTokenRequest BuildOAuthRefreshTokenRequest(const OAuthRefreshTokenRequestInput& input);
OAuthTokenResponse ParseOAuthTokenResponse(const std::string& response_json);
OAuthTokenResponse ExecuteOAuthTokenExchange(
    const CliHost& cli_host,
    const OAuthTokenRequestInput& input,
    const std::filesystem::path& workspace_path,
    int timeout_ms = 10000);
OAuthTokenResponse ExecuteOAuthRefreshTokenExchange(
    const CliHost& cli_host,
    const OAuthRefreshTokenRequestInput& input,
    const std::filesystem::path& workspace_path,
    int timeout_ms = 10000);
AuthSession PersistOAuthTokenSession(
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const OAuthSessionPersistInput& input);
AuthSession PersistOAuthRefreshSession(
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const AuthSession& existing_session,
    const OAuthTokenResponse& token_response);
AuthSession CompleteOAuthLogin(
    const CliHost& cli_host,
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const OAuthLoginOrchestrationInput& input,
    const std::filesystem::path& workspace_path,
    int timeout_ms = 10000);
AuthSession RefreshOAuthSession(
    const CliHost& cli_host,
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const OAuthRefreshOrchestrationInput& input,
    const std::filesystem::path& workspace_path,
    int timeout_ms = 10000);

std::string CreatePkceCodeChallengeForTest(const std::string& verifier);
std::string ExtractRedirectUriPathForTest(const std::string& redirect_uri);
std::string TargetPathOnlyForTest(const std::string& target);
std::optional<std::string> ExtractHttpHostHeaderForTest(const std::string& request_text);
bool HostHeaderMatchesLoopbackForTest(const std::string& host_header, int port);

}  // namespace agentos
