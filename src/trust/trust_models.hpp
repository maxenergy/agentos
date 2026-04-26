#pragma once

#include <string>
#include <vector>

namespace agentos {

enum class TrustLevel {
    local,
    paired,
    blocked,
};

struct Identity {
    std::string identity_id;
    std::string user_id;
    std::string label;
};

struct TrustedPeer {
    std::string identity_id;
    std::string device_id;
    std::string label;
    TrustLevel trust_level = TrustLevel::paired;
    std::vector<std::string> permissions;
    long long paired_epoch_ms = 0;
    long long last_seen_epoch_ms = 0;
};

struct PairingInvite {
    std::string token;
    std::string identity_id;
    std::string device_id;
    std::string label;
    std::string user_id;
    std::string identity_label;
    std::vector<std::string> permissions;
    long long created_epoch_ms = 0;
    long long expires_epoch_ms = 0;
    bool consumed = false;
};

std::string ToString(TrustLevel trust_level);
TrustLevel ParseTrustLevel(const std::string& value);

}  // namespace agentos
