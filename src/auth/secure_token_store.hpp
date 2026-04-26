#pragma once

#include <map>
#include <memory>
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

// Backend abstraction for managed token storage.  Production backends bind to a
// platform credential store (Windows Credential Manager, macOS Keychain, Linux
// Secret Service), and a test-only in-memory backend can be installed for unit
// tests so we never reach the real keychain in CI.
class ISecureTokenBackend {
public:
    virtual ~ISecureTokenBackend() = default;
    virtual SecureTokenStoreStatus status() const = 0;
    virtual std::string ref_prefix() const = 0;
    virtual std::string make_ref(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name) const = 0;
    virtual std::string write(
        const std::string& provider,
        const std::string& profile_name,
        const std::string& token_name,
        const std::string& token_value) = 0;
    virtual std::optional<std::string> read(const std::string& token_ref) const = 0;
    virtual bool remove(const std::string& token_ref) = 0;
    virtual bool owns_ref(const std::string& token_ref) const = 0;
};

class SecureTokenStore {
public:
    SecureTokenStore();
    explicit SecureTokenStore(std::shared_ptr<ISecureTokenBackend> backend);

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

    // Returns the active backend name (e.g. "windows-credential-manager",
    // "macos-keychain", "linux-secret-service", "in-memory", "env-ref-only").
    std::string backend_name() const;

    // Test/diagnostic-only: install an alternative backend for managed tokens.
    // Calling code keeps ownership-by-shared-ptr.  Passing a null pointer
    // re-selects the platform default backend.
    void set_backend_for_testing(std::shared_ptr<ISecureTokenBackend> backend);

    // Construct an in-memory backend suitable for unit tests.  Tokens live for
    // the lifetime of the returned shared_ptr; nothing touches the real OS
    // credential store.
    static std::shared_ptr<ISecureTokenBackend> MakeInMemoryBackendForTesting();

private:
    std::shared_ptr<ISecureTokenBackend> backend_;
};

}  // namespace agentos
