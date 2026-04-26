#include "auth/secure_token_store.hpp"

#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#include <Windows.h>
#include <wincred.h>
#endif

namespace agentos {

namespace {

constexpr auto kEnvPrefix = "env:";
constexpr auto kWindowsCredentialPrefix = "wincred:";

std::string ManagedCredentialName(
    const std::string& provider,
    const std::string& profile_name,
    const std::string& token_name) {
    return "AgentOS/" + provider + "/" + profile_name + "/" + token_name;
}

#ifdef _WIN32
std::optional<std::string> ReadWindowsCredential(const std::string& target_name) {
    PCREDENTIALA credential = nullptr;
    if (!CredReadA(target_name.c_str(), CRED_TYPE_GENERIC, 0, &credential) || credential == nullptr) {
        return std::nullopt;
    }

    std::string value;
    if (credential->CredentialBlobSize > 0 && credential->CredentialBlob != nullptr) {
        value.assign(
            reinterpret_cast<const char*>(credential->CredentialBlob),
            reinterpret_cast<const char*>(credential->CredentialBlob) + credential->CredentialBlobSize);
    }
    CredFree(credential);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

void WriteWindowsCredential(const std::string& target_name, const std::string& token_value) {
    CREDENTIALA credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPSTR>(target_name.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(token_value.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(token_value.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (!CredWriteA(&credential, 0)) {
        throw std::runtime_error("failed to write token to Windows Credential Manager");
    }
}
#endif

}  // namespace

std::string SecureTokenStore::make_env_ref(const std::string& env_name) const {
    return std::string(kEnvPrefix) + env_name;
}

std::string SecureTokenStore::make_managed_ref(
    const std::string& provider,
    const std::string& profile_name,
    const std::string& token_name) const {
#ifdef _WIN32
    return std::string(kWindowsCredentialPrefix) + ManagedCredentialName(provider, profile_name, token_name);
#else
    (void)provider;
    (void)profile_name;
    (void)token_name;
    throw std::runtime_error("managed token storage requires a system credential store backend");
#endif
}

std::string SecureTokenStore::write_managed_token(
    const std::string& provider,
    const std::string& profile_name,
    const std::string& token_name,
    const std::string& token_value) const {
    if (token_value.empty()) {
        throw std::invalid_argument("managed token value must not be empty");
    }
#ifdef _WIN32
    const auto target_name = ManagedCredentialName(provider, profile_name, token_name);
    WriteWindowsCredential(target_name, token_value);
    return std::string(kWindowsCredentialPrefix) + target_name;
#else
    (void)provider;
    (void)profile_name;
    (void)token_name;
    (void)token_value;
    throw std::runtime_error("managed token storage requires a system credential store backend");
#endif
}

std::optional<std::string> SecureTokenStore::read_ref(const std::string& token_ref) const {
    if (token_ref.rfind(kEnvPrefix, 0) == 0) {
        const auto env_name = token_ref.substr(std::string(kEnvPrefix).size());
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

#ifdef _WIN32
    if (token_ref.rfind(kWindowsCredentialPrefix, 0) == 0) {
        return ReadWindowsCredential(token_ref.substr(std::string(kWindowsCredentialPrefix).size()));
    }
#endif

    return std::nullopt;
}

bool SecureTokenStore::delete_ref(const std::string& token_ref) const {
#ifdef _WIN32
    if (token_ref.rfind(kWindowsCredentialPrefix, 0) == 0) {
        const auto target_name = token_ref.substr(std::string(kWindowsCredentialPrefix).size());
        if (CredDeleteA(target_name.c_str(), CRED_TYPE_GENERIC, 0)) {
            return true;
        }
        return GetLastError() == ERROR_NOT_FOUND;
    }
#endif
    return false;
}

bool SecureTokenStore::ref_available(const std::string& token_ref) const {
    return read_ref(token_ref).has_value();
}

SecureTokenStoreStatus SecureTokenStore::status() const {
#ifdef _WIN32
    return {
        .backend_name = "windows-credential-manager",
        .system_keychain_backed = true,
        .stores_plaintext = false,
        .dev_only = false,
        .message = "Managed tokens can be stored in Windows Credential Manager; env: references remain supported.",
    };
#else
    return {
        .backend_name = "env-ref-only",
        .system_keychain_backed = false,
        .stores_plaintext = false,
        .dev_only = true,
        .message = "MVP fallback only stores environment-variable references; use a real system credential store before storing managed secrets.",
    };
#endif
}

}  // namespace agentos
