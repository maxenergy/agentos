#include "auth/secure_token_store.hpp"

#include <cstdlib>

namespace agentos {

std::string SecureTokenStore::make_env_ref(const std::string& env_name) const {
    return "env:" + env_name;
}

std::optional<std::string> SecureTokenStore::read_ref(const std::string& token_ref) const {
    constexpr auto prefix = "env:";
    if (token_ref.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    const auto env_name = token_ref.substr(4);
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, env_name.c_str()) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }

    std::string value(raw_value, value_size > 0 ? value_size - 1 : 0);
    std::free(raw_value);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(env_name.c_str());
    if (!value || std::string(value).empty()) {
        return std::nullopt;
    }

    return std::string(value);
#endif
}

bool SecureTokenStore::ref_available(const std::string& token_ref) const {
    return read_ref(token_ref).has_value();
}

SecureTokenStoreStatus SecureTokenStore::status() const {
    return {
        .backend_name = "env-ref-only",
        .system_keychain_backed = false,
        .stores_plaintext = false,
        .dev_only = true,
        .message = "MVP fallback only stores environment-variable references; use a real system credential store before storing managed secrets.",
    };
}

}  // namespace agentos
