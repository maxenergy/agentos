#include "trust/trust_models.hpp"

namespace agentos {

std::string ToString(const TrustLevel trust_level) {
    switch (trust_level) {
    case TrustLevel::local:
        return "local";
    case TrustLevel::paired:
        return "paired";
    case TrustLevel::blocked:
        return "blocked";
    default:
        return "blocked";
    }
}

TrustLevel ParseTrustLevel(const std::string& value) {
    if (value == "local") {
        return TrustLevel::local;
    }
    if (value == "paired") {
        return TrustLevel::paired;
    }
    return TrustLevel::blocked;
}

}  // namespace agentos

