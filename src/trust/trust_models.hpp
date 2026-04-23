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
};

std::string ToString(TrustLevel trust_level);
TrustLevel ParseTrustLevel(const std::string& value);

}  // namespace agentos

