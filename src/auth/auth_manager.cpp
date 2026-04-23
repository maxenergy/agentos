#include "auth/auth_manager.hpp"

#include <stdexcept>

namespace agentos {

AuthManager::AuthManager(SessionStore& session_store)
    : session_store_(session_store) {}

void AuthManager::register_provider(std::shared_ptr<IAuthProviderAdapter> adapter) {
    if (!adapter) {
        return;
    }

    const auto provider = adapter->descriptor().provider;
    const auto key = ToString(provider);
    if (!providers_.contains(key)) {
        registration_order_.push_back(provider);
    }

    providers_[key] = std::move(adapter);
}

std::vector<AuthProviderDescriptor> AuthManager::providers() const {
    std::vector<AuthProviderDescriptor> descriptors;
    descriptors.reserve(registration_order_.size());

    for (const auto provider : registration_order_) {
        const auto adapter = find(provider);
        if (adapter) {
            descriptors.push_back(adapter->descriptor());
        }
    }

    return descriptors;
}

AuthSession AuthManager::login(
    const AuthProviderId provider,
    const AuthMode mode,
    const std::map<std::string, std::string>& options) {
    const auto adapter = find(provider);
    if (!adapter) {
        throw std::runtime_error("ProviderNotRegistered");
    }

    return adapter->login(mode, options);
}

AuthStatus AuthManager::status(const AuthProviderId provider, const std::string& profile_name) {
    const auto adapter = find(provider);
    if (!adapter) {
        throw std::runtime_error("ProviderNotRegistered");
    }

    return adapter->status(profile_name);
}

std::vector<AuthStatus> AuthManager::status_all(const std::string& profile_name) {
    std::vector<AuthStatus> statuses;
    statuses.reserve(registration_order_.size());

    for (const auto provider : registration_order_) {
        statuses.push_back(status(provider, profile_name));
    }

    return statuses;
}

std::optional<AuthSession> AuthManager::probe(const AuthProviderId provider) {
    const auto adapter = find(provider);
    if (!adapter) {
        throw std::runtime_error("ProviderNotRegistered");
    }

    return adapter->probe_external_session();
}

void AuthManager::logout(const AuthProviderId provider, const std::string& profile_name) {
    const auto adapter = find(provider);
    if (!adapter) {
        throw std::runtime_error("ProviderNotRegistered");
    }

    adapter->logout(profile_name);
}

std::shared_ptr<IAuthProviderAdapter> AuthManager::find(const AuthProviderId provider) const {
    const auto it = providers_.find(ToString(provider));
    if (it == providers_.end()) {
        return nullptr;
    }

    return it->second;
}

}  // namespace agentos

