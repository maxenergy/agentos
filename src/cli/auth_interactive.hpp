#pragma once

#include "auth/auth_models.hpp"
#include "auth/oauth_pkce.hpp"

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

// Resolution emitted by PromptInteractiveLogin: either a fully-populated set of
// answers ready to be turned into an `auth login` argv, or an error explaining
// why the prompt sequence aborted.  The browser_oauth case is intentionally
// modelled as `ok=false` with `redirect_to_oauth_login=true`; callers should
// surface the message and exit non-zero rather than try to embed a PKCE flow.
struct InteractiveLoginResolution {
    bool ok = false;
    bool redirect_to_oauth_login = false;
    std::string error_message;
    AuthProviderId provider = AuthProviderId::openai;
    AuthMode mode = AuthMode::api_key;
    std::string profile_name = "default";
    std::string api_key_env;
    bool set_default = true;
};

// Drives the interactive login prompt sequence using the supplied streams.
// Stdin is consumed line-at-a-time; EOF returns ok=false with an explanatory
// message instead of looping forever.  The mode list is computed from the
// provider's AuthProviderDescriptor and the OAuth-defaults metadata returned by
// `defaults_fn` so callers can plug in either the builtin or workspace-merged
// view.
//
// All prompts are written to `out` and flushed; tests pass a std::stringstream
// to capture the rendered transcript.
InteractiveLoginResolution PromptInteractiveLogin(
    const std::vector<AuthProviderDescriptor>& providers,
    const std::function<OAuthProviderDefaults(AuthProviderId)>& defaults_fn,
    std::optional<AuthProviderId> preselected_provider,
    std::istream& in,
    std::ostream& out);

// Builds the argv vector that the existing `auth login` handler would parse
// for the resolved interactive answers.  Exposed for testability; the
// CLI dispatcher uses this to re-enter `RunAuthCommand`.
std::vector<std::string> BuildLoginArgvFromResolution(
    const InteractiveLoginResolution& resolution);

// Default API-key environment variable hint for a provider, e.g.
// AGENTOS_QWEN_API_KEY.  Exposed for testability.
std::string DefaultApiKeyEnvName(AuthProviderId provider);

}  // namespace agentos
