#pragma once

#include "trust/allowlist_store.hpp"

#include <optional>
#include <string>
#include <vector>

namespace agentos {

class PairingManager {
public:
    explicit PairingManager(AllowlistStore& allowlist_store);

    TrustedPeer pair(
        const std::string& identity_id,
        const std::string& device_id,
        const std::string& label,
        std::vector<std::string> permissions);
    void block(const std::string& identity_id, const std::string& device_id);
    bool unblock(const std::string& identity_id, const std::string& device_id);
    bool rename_device(const std::string& identity_id, const std::string& device_id, const std::string& label);
    bool mark_seen(const std::string& identity_id, const std::string& device_id);
    void remove(const std::string& identity_id, const std::string& device_id);
    std::optional<TrustedPeer> find(const std::string& identity_id, const std::string& device_id) const;
    std::vector<TrustedPeer> list() const;

    static long long NowEpochMs();

private:
    AllowlistStore& allowlist_store_;
};

}  // namespace agentos
