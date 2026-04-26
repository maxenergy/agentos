#pragma once

#include <map>
#include <optional>
#include <string>

namespace agentos {

struct SecureTokenStoreStatus {
    std::string backend_name;
    bool system_keychain_backed = false;
    bool stores_plaintext = false;
    bool dev_only = true;
    std::string message;
};

class SecureTokenStore {
public:
    std::string make_env_ref(const std::string& env_name) const;
    std::string make_managed_ref(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name) const;
    std::string write_managed_token(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name,
        const std::string& token_value) const;
    std::optional<std::string> read_ref(const std::string& token_ref) const;
    bool delete_ref(const std::string& token_ref) const;
    bool ref_available(const std::string& token_ref) const;
    SecureTokenStoreStatus status() const;
};

}  // namespace agentos
