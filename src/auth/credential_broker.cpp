#include "auth/credential_broker.hpp"

#include <stdexcept>

namespace agentos {

CredentialBroker::CredentialBroker(SessionStore& session_store, SecureTokenStore& token_store)
    : session_store_(session_store),
      token_store_(token_store) {}

std::optional<AuthSession> CredentialBroker::get_session(const AuthProviderId provider, const std::string& profile_name) const {
    const auto session = session_store_.find(provider, profile_name);
    if (!session.has_value() || IsAuthSessionExpired(*session)) {
        return std::nullopt;
    }
    return session;
}

std::string CredentialBroker::get_access_token(const AuthProviderId provider, const std::string& profile_name) const {
    const auto session = get_session(provider, profile_name);
    if (!session.has_value()) {
        throw std::runtime_error("auth session is not available");
    }

    const auto value = token_store_.read_ref(session->access_token_ref);
    if (!value.has_value()) {
        throw std::runtime_error("credential reference is not available");
    }

    return *value;
}

}  // namespace agentos

