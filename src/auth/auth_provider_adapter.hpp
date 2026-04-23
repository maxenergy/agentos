#pragma once

#include "auth/auth_models.hpp"

#include <map>
#include <optional>

namespace agentos {

class IAuthProviderAdapter {
public:
    virtual AuthProviderDescriptor descriptor() const = 0;
    virtual std::vector<AuthMode> supported_modes() const = 0;
    virtual AuthSession login(AuthMode mode, const std::map<std::string, std::string>& options) = 0;
    virtual AuthStatus status(const std::string& profile_name) = 0;
    virtual AuthSession refresh(const AuthSession& session) = 0;
    virtual void logout(const std::string& profile_name) = 0;
    virtual std::optional<AuthSession> probe_external_session() = 0;
    virtual ~IAuthProviderAdapter() = default;
};

}  // namespace agentos

