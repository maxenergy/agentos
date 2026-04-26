#include "auth/secure_token_store.hpp"

#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include <Windows.h>
#include <wincred.h>
#endif

#ifdef __APPLE__
#include <Security/Security.h>
#endif

#ifdef AGENTOS_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace agentos {

namespace {

constexpr auto kEnvPrefix = "env:";
constexpr auto kWindowsCredentialPrefix = "wincred:";
constexpr auto kMacKeychainPrefix = "keychain:";
constexpr auto kLinuxSecretPrefix = "secret-service:";
constexpr auto kInMemoryPrefix = "memtoken:";
constexpr auto kAgentOsService = "AgentOS";

std::string ManagedCredentialName(
    const std::string& provider,
    const std::string& profile_name,
    const std::string& token_name) {
    return std::string(kAgentOsService) + "/" + provider + "/" + profile_name + "/" + token_name;
}

std::optional<std::string> ReadEnvVariable(const std::string& env_name) {
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

// ---------------------------------------------------------------------------
// Env-only fallback backend (always available).
// ---------------------------------------------------------------------------
class EnvRefOnlyBackend final : public ISecureTokenBackend {
public:
    SecureTokenStoreStatus status() const override {
        return {
            .backend_name = "env-ref-only",
            .system_keychain_backed = false,
            .stores_plaintext = false,
            .dev_only = true,
            .message = "MVP fallback only stores environment-variable references; install a real "
                       "system credential store before storing managed secrets.",
        };
    }
    std::string ref_prefix() const override { return ""; }
    std::string make_ref(const std::string&, const std::string&, const std::string&) const override {
        throw std::runtime_error("PolicyDenied: managed token storage requires a system credential store backend");
    }
    std::string write(const std::string&, const std::string&, const std::string&, const std::string&) override {
        throw std::runtime_error("PolicyDenied: managed token storage requires a system credential store backend");
    }
    std::optional<std::string> read(const std::string&) const override { return std::nullopt; }
    bool remove(const std::string&) override { return false; }
    bool owns_ref(const std::string&) const override { return false; }
};

// ---------------------------------------------------------------------------
// In-memory backend for tests.
// ---------------------------------------------------------------------------
class InMemoryBackend final : public ISecureTokenBackend {
public:
    SecureTokenStoreStatus status() const override {
        return {
            .backend_name = "in-memory",
            .system_keychain_backed = false,
            .stores_plaintext = false,
            .dev_only = true,
            .message = "In-memory backend for tests; tokens are not persisted and never reach a real keychain.",
        };
    }
    std::string ref_prefix() const override { return kInMemoryPrefix; }
    std::string make_ref(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name) const override {
        return std::string(kInMemoryPrefix) + ManagedCredentialName(provider, profile_name, token_name);
    }
    std::string write(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name,
        const std::string& token_value) override {
        const auto ref = make_ref(provider, profile_name, token_name);
        std::lock_guard<std::mutex> lock(mu_);
        store_[ref] = token_value;
        return ref;
    }
    std::optional<std::string> read(const std::string& token_ref) const override {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = store_.find(token_ref);
        if (it == store_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    bool remove(const std::string& token_ref) override {
        std::lock_guard<std::mutex> lock(mu_);
        return store_.erase(token_ref) > 0;
    }
    bool owns_ref(const std::string& token_ref) const override {
        return token_ref.rfind(kInMemoryPrefix, 0) == 0;
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, std::string> store_;
};

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows Credential Manager backend.
// ---------------------------------------------------------------------------
class WindowsCredentialBackend final : public ISecureTokenBackend {
public:
    SecureTokenStoreStatus status() const override {
        return {
            .backend_name = "windows-credential-manager",
            .system_keychain_backed = true,
            .stores_plaintext = false,
            .dev_only = false,
            .message = "Managed tokens are stored in Windows Credential Manager; env: references remain supported.",
        };
    }
    std::string ref_prefix() const override { return kWindowsCredentialPrefix; }
    std::string make_ref(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name) const override {
        return std::string(kWindowsCredentialPrefix) + ManagedCredentialName(provider, profile_name, token_name);
    }
    std::string write(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name,
        const std::string& token_value) override {
        const auto target_name = ManagedCredentialName(provider, profile_name, token_name);
        CREDENTIALA credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<LPSTR>(target_name.c_str());
        credential.CredentialBlobSize = static_cast<DWORD>(token_value.size());
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(token_value.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteA(&credential, 0)) {
            throw std::runtime_error("ExternalProcessFailed: failed to write token to Windows Credential Manager");
        }
        return std::string(kWindowsCredentialPrefix) + target_name;
    }
    std::optional<std::string> read(const std::string& token_ref) const override {
        if (token_ref.rfind(kWindowsCredentialPrefix, 0) != 0) {
            return std::nullopt;
        }
        const auto target_name = token_ref.substr(std::string(kWindowsCredentialPrefix).size());
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
    bool remove(const std::string& token_ref) override {
        if (token_ref.rfind(kWindowsCredentialPrefix, 0) != 0) {
            return false;
        }
        const auto target_name = token_ref.substr(std::string(kWindowsCredentialPrefix).size());
        if (CredDeleteA(target_name.c_str(), CRED_TYPE_GENERIC, 0)) {
            return true;
        }
        return GetLastError() == ERROR_NOT_FOUND;
    }
    bool owns_ref(const std::string& token_ref) const override {
        return token_ref.rfind(kWindowsCredentialPrefix, 0) == 0;
    }
};
#endif  // _WIN32

#ifdef __APPLE__
// ---------------------------------------------------------------------------
// macOS Keychain backend (Security.framework, generic password items).
// ---------------------------------------------------------------------------
class MacKeychainBackend final : public ISecureTokenBackend {
public:
    SecureTokenStoreStatus status() const override {
        return {
            .backend_name = "macos-keychain",
            .system_keychain_backed = true,
            .stores_plaintext = false,
            .dev_only = false,
            .message = "Managed tokens are stored in the macOS Keychain (Security framework, generic password items); "
                       "env: references remain supported.",
        };
    }
    std::string ref_prefix() const override { return kMacKeychainPrefix; }
    std::string make_ref(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name) const override {
        return std::string(kMacKeychainPrefix) + ManagedCredentialName(provider, profile_name, token_name);
    }
    std::string write(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name,
        const std::string& token_value) override {
        const auto account = ManagedCredentialName(provider, profile_name, token_name);
        // Try to update an existing item first; if none exists, add it.
        OSStatus status = SecKeychainAddGenericPassword(
            nullptr,
            static_cast<UInt32>(std::string(kAgentOsService).size()), kAgentOsService,
            static_cast<UInt32>(account.size()), account.c_str(),
            static_cast<UInt32>(token_value.size()), token_value.data(),
            nullptr);
        if (status == errSecDuplicateItem) {
            SecKeychainItemRef item = nullptr;
            status = SecKeychainFindGenericPassword(
                nullptr,
                static_cast<UInt32>(std::string(kAgentOsService).size()), kAgentOsService,
                static_cast<UInt32>(account.size()), account.c_str(),
                nullptr, nullptr, &item);
            if (status == errSecSuccess && item != nullptr) {
                status = SecKeychainItemModifyAttributesAndData(
                    item, nullptr,
                    static_cast<UInt32>(token_value.size()), token_value.data());
                CFRelease(item);
            }
        }
        if (status != errSecSuccess) {
            throw std::runtime_error("ExternalProcessFailed: failed to write token to macOS Keychain");
        }
        return std::string(kMacKeychainPrefix) + account;
    }
    std::optional<std::string> read(const std::string& token_ref) const override {
        if (token_ref.rfind(kMacKeychainPrefix, 0) != 0) {
            return std::nullopt;
        }
        const auto account = token_ref.substr(std::string(kMacKeychainPrefix).size());
        UInt32 length = 0;
        void* data = nullptr;
        const OSStatus status = SecKeychainFindGenericPassword(
            nullptr,
            static_cast<UInt32>(std::string(kAgentOsService).size()), kAgentOsService,
            static_cast<UInt32>(account.size()), account.c_str(),
            &length, &data, nullptr);
        if (status != errSecSuccess || data == nullptr) {
            return std::nullopt;
        }
        std::string value(static_cast<const char*>(data), static_cast<std::size_t>(length));
        SecKeychainItemFreeContent(nullptr, data);
        if (value.empty()) {
            return std::nullopt;
        }
        return value;
    }
    bool remove(const std::string& token_ref) override {
        if (token_ref.rfind(kMacKeychainPrefix, 0) != 0) {
            return false;
        }
        const auto account = token_ref.substr(std::string(kMacKeychainPrefix).size());
        SecKeychainItemRef item = nullptr;
        OSStatus status = SecKeychainFindGenericPassword(
            nullptr,
            static_cast<UInt32>(std::string(kAgentOsService).size()), kAgentOsService,
            static_cast<UInt32>(account.size()), account.c_str(),
            nullptr, nullptr, &item);
        if (status == errSecItemNotFound) {
            return true;
        }
        if (status != errSecSuccess || item == nullptr) {
            return false;
        }
        status = SecKeychainItemDelete(item);
        CFRelease(item);
        return status == errSecSuccess;
    }
    bool owns_ref(const std::string& token_ref) const override {
        return token_ref.rfind(kMacKeychainPrefix, 0) == 0;
    }
};
#endif  // __APPLE__

#ifdef AGENTOS_HAVE_LIBSECRET
// ---------------------------------------------------------------------------
// Linux Secret Service backend via libsecret (D-Bus).
// ---------------------------------------------------------------------------
class LinuxSecretServiceBackend final : public ISecureTokenBackend {
public:
    SecureTokenStoreStatus status() const override {
        return {
            .backend_name = "linux-secret-service",
            .system_keychain_backed = true,
            .stores_plaintext = false,
            .dev_only = false,
            .message = "Managed tokens are stored via the freedesktop Secret Service (libsecret/D-Bus); "
                       "env: references remain supported.",
        };
    }
    std::string ref_prefix() const override { return kLinuxSecretPrefix; }
    std::string make_ref(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name) const override {
        return std::string(kLinuxSecretPrefix) + ManagedCredentialName(provider, profile_name, token_name);
    }
    std::string write(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name,
        const std::string& token_value) override {
        const auto account = ManagedCredentialName(provider, profile_name, token_name);
        GError* error = nullptr;
        const gboolean ok = secret_password_store_sync(
            schema_(),
            SECRET_COLLECTION_DEFAULT,
            ("AgentOS managed token: " + account).c_str(),
            token_value.c_str(),
            nullptr,
            &error,
            "service", kAgentOsService,
            "account", account.c_str(),
            nullptr);
        if (!ok) {
            std::string message = "ExternalProcessFailed: failed to write token to Secret Service";
            if (error != nullptr) {
                message += ": ";
                message += error->message;
                g_error_free(error);
            }
            throw std::runtime_error(message);
        }
        return std::string(kLinuxSecretPrefix) + account;
    }
    std::optional<std::string> read(const std::string& token_ref) const override {
        if (token_ref.rfind(kLinuxSecretPrefix, 0) != 0) {
            return std::nullopt;
        }
        const auto account = token_ref.substr(std::string(kLinuxSecretPrefix).size());
        GError* error = nullptr;
        gchar* secret = secret_password_lookup_sync(
            schema_(),
            nullptr,
            &error,
            "service", kAgentOsService,
            "account", account.c_str(),
            nullptr);
        if (error != nullptr) {
            g_error_free(error);
            return std::nullopt;
        }
        if (secret == nullptr) {
            return std::nullopt;
        }
        std::string value(secret);
        secret_password_free(secret);
        if (value.empty()) {
            return std::nullopt;
        }
        return value;
    }
    bool remove(const std::string& token_ref) override {
        if (token_ref.rfind(kLinuxSecretPrefix, 0) != 0) {
            return false;
        }
        const auto account = token_ref.substr(std::string(kLinuxSecretPrefix).size());
        GError* error = nullptr;
        const gboolean ok = secret_password_clear_sync(
            schema_(),
            nullptr,
            &error,
            "service", kAgentOsService,
            "account", account.c_str(),
            nullptr);
        if (error != nullptr) {
            g_error_free(error);
        }
        return ok != 0;
    }
    bool owns_ref(const std::string& token_ref) const override {
        return token_ref.rfind(kLinuxSecretPrefix, 0) == 0;
    }

private:
    static const SecretSchema* schema_() {
        static const SecretSchema schema = {
            "io.agentos.AgentOS",
            SECRET_SCHEMA_NONE,
            {
                {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
                {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
                {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
            },
            // padding zero-init handled by static
            0, 0, 0, 0, 0, 0, 0, 0,
        };
        return &schema;
    }
};
#endif  // AGENTOS_HAVE_LIBSECRET

std::shared_ptr<ISecureTokenBackend> MakePlatformBackend() {
#if defined(_WIN32)
    return std::make_shared<WindowsCredentialBackend>();
#elif defined(__APPLE__)
    return std::make_shared<MacKeychainBackend>();
#elif defined(AGENTOS_HAVE_LIBSECRET)
    return std::make_shared<LinuxSecretServiceBackend>();
#else
    return std::make_shared<EnvRefOnlyBackend>();
#endif
}

}  // namespace

SecureTokenStore::SecureTokenStore() : backend_(MakePlatformBackend()) {}

SecureTokenStore::SecureTokenStore(std::shared_ptr<ISecureTokenBackend> backend)
    : backend_(backend ? std::move(backend) : MakePlatformBackend()) {}

std::string SecureTokenStore::make_env_ref(const std::string& env_name) const {
    return std::string(kEnvPrefix) + env_name;
}

std::string SecureTokenStore::make_managed_ref(
    const std::string& provider,
    const std::string& profile_name,
    const std::string& token_name) const {
    return backend_->make_ref(provider, profile_name, token_name);
}

std::string SecureTokenStore::write_managed_token(
    const std::string& provider,
    const std::string& profile_name,
    const std::string& token_name,
    const std::string& token_value) const {
    if (token_value.empty()) {
        throw std::invalid_argument("PolicyDenied: managed token value must not be empty");
    }
    return backend_->write(provider, profile_name, token_name, token_value);
}

std::optional<std::string> SecureTokenStore::read_ref(const std::string& token_ref) const {
    if (token_ref.rfind(kEnvPrefix, 0) == 0) {
        const auto env_name = token_ref.substr(std::string(kEnvPrefix).size());
        return ReadEnvVariable(env_name);
    }
    if (backend_->owns_ref(token_ref)) {
        return backend_->read(token_ref);
    }
    return std::nullopt;
}

bool SecureTokenStore::delete_ref(const std::string& token_ref) const {
    if (backend_->owns_ref(token_ref)) {
        return backend_->remove(token_ref);
    }
    return false;
}

bool SecureTokenStore::ref_available(const std::string& token_ref) const {
    return read_ref(token_ref).has_value();
}

SecureTokenStoreStatus SecureTokenStore::status() const {
    return backend_->status();
}

std::string SecureTokenStore::backend_name() const {
    return backend_->status().backend_name;
}

void SecureTokenStore::set_backend_for_testing(std::shared_ptr<ISecureTokenBackend> backend) {
    backend_ = backend ? std::move(backend) : MakePlatformBackend();
}

std::shared_ptr<ISecureTokenBackend> SecureTokenStore::MakeInMemoryBackendForTesting() {
    return std::make_shared<InMemoryBackend>();
}

}  // namespace agentos
