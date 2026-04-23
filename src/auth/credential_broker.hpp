#pragma once

#include "auth/auth_models.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"

#include <optional>
#include <string>

namespace agentos {

class CredentialBroker {
public:
    CredentialBroker(SessionStore& session_store, SecureTokenStore& token_store);

    std::optional<AuthSession> get_session(AuthProviderId provider, const std::string& profile_name) const;
    std::string get_access_token(AuthProviderId provider, const std::string& profile_name) const;

private:
    SessionStore& session_store_;
    SecureTokenStore& token_store_;
};

}  // namespace agentos

