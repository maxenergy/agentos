#pragma once

#include "auth/auth_provider_adapter.hpp"
#include "auth/session_store.hpp"

#include <memory>
#include <optional>
#include <unordered_map>

namespace agentos {

class AuthManager {
public:
    explicit AuthManager(SessionStore& session_store);

    void register_provider(std::shared_ptr<IAuthProviderAdapter> adapter);
    std::vector<AuthProviderDescriptor> providers() const;
    AuthSession login(AuthProviderId provider, AuthMode mode, const std::map<std::string, std::string>& options);
    AuthStatus status(AuthProviderId provider, const std::string& profile_name);
    std::vector<AuthStatus> status_all(const std::string& profile_name);
    std::optional<AuthSession> probe(AuthProviderId provider);
    void logout(AuthProviderId provider, const std::string& profile_name);

private:
    std::shared_ptr<IAuthProviderAdapter> find(AuthProviderId provider) const;

    SessionStore& session_store_;
    std::unordered_map<std::string, std::shared_ptr<IAuthProviderAdapter>> providers_;
    std::vector<AuthProviderId> registration_order_;
};

}  // namespace agentos

