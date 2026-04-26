#include "trust/pairing_manager.hpp"

#include <chrono>
#include <utility>

namespace agentos {

PairingManager::PairingManager(AllowlistStore& allowlist_store)
    : allowlist_store_(allowlist_store) {}

TrustedPeer PairingManager::pair(
    const std::string& identity_id,
    const std::string& device_id,
    const std::string& label,
    std::vector<std::string> permissions) {
    const auto now = NowEpochMs();
    const auto existing = allowlist_store_.find(identity_id, device_id);
    TrustedPeer peer{
        .identity_id = identity_id,
        .device_id = device_id,
        .label = label,
        .trust_level = TrustLevel::paired,
        .permissions = std::move(permissions),
        .paired_epoch_ms = existing.has_value() && existing->paired_epoch_ms > 0 ? existing->paired_epoch_ms : now,
        .last_seen_epoch_ms = now,
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
    if (peer.paired_epoch_ms == 0) {
        peer.paired_epoch_ms = NowEpochMs();
    }
    allowlist_store_.save(peer);
}

bool PairingManager::unblock(const std::string& identity_id, const std::string& device_id) {
    auto peer = allowlist_store_.find(identity_id, device_id);
    if (!peer.has_value()) {
        return false;
    }
    peer->trust_level = TrustLevel::paired;
    peer->last_seen_epoch_ms = NowEpochMs();
    allowlist_store_.save(*peer);
    return true;
}

bool PairingManager::rename_device(
    const std::string& identity_id,
    const std::string& device_id,
    const std::string& label) {
    auto peer = allowlist_store_.find(identity_id, device_id);
    if (!peer.has_value()) {
        return false;
    }
    peer->label = label;
    allowlist_store_.save(*peer);
    return true;
}

bool PairingManager::mark_seen(const std::string& identity_id, const std::string& device_id) {
    auto peer = allowlist_store_.find(identity_id, device_id);
    if (!peer.has_value()) {
        return false;
    }
    peer->last_seen_epoch_ms = NowEpochMs();
    allowlist_store_.save(*peer);
    return true;
}

void PairingManager::remove(const std::string& identity_id, const std::string& device_id) {
    allowlist_store_.remove(identity_id, device_id);
}

std::optional<TrustedPeer> PairingManager::find(const std::string& identity_id, const std::string& device_id) const {
    return allowlist_store_.find(identity_id, device_id);
}

std::vector<TrustedPeer> PairingManager::list() const {
    return allowlist_store_.list();
}

long long PairingManager::NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace agentos
