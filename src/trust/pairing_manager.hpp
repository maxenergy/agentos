#pragma once

#include "trust/allowlist_store.hpp"

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
    void remove(const std::string& identity_id, const std::string& device_id);
    std::vector<TrustedPeer> list() const;

private:
    AllowlistStore& allowlist_store_;
};

}  // namespace agentos

