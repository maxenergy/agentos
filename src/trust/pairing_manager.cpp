#include "trust/pairing_manager.hpp"

#include <utility>

namespace agentos {

PairingManager::PairingManager(AllowlistStore& allowlist_store)
    : allowlist_store_(allowlist_store) {}

TrustedPeer PairingManager::pair(
    const std::string& identity_id,
    const std::string& device_id,
    const std::string& label,
    std::vector<std::string> permissions) {
    TrustedPeer peer{
        .identity_id = identity_id,
        .device_id = device_id,
        .label = label,
        .trust_level = TrustLevel::paired,
        .permissions = std::move(permissions),
    };

    allowlist_store_.save(peer);
    return peer;
}

void PairingManager::block(const std::string& identity_id, const std::string& device_id) {
    auto peer = allowlist_store_.find(identity_id, device_id).value_or(TrustedPeer{
        .identity_id = identity_id,
        .device_id = device_id,
        .label = "blocked",
    });
    peer.trust_level = TrustLevel::blocked;
    allowlist_store_.save(peer);
}

void PairingManager::remove(const std::string& identity_id, const std::string& device_id) {
    allowlist_store_.remove(identity_id, device_id);
}

std::vector<TrustedPeer> PairingManager::list() const {
    return allowlist_store_.list();
}

}  // namespace agentos
