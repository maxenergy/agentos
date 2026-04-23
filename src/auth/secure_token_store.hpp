#pragma once

#include <map>
#include <optional>
#include <string>

namespace agentos {

class SecureTokenStore {
public:
    std::string make_env_ref(const std::string& env_name) const;
    std::optional<std::string> read_ref(const std::string& token_ref) const;
    bool ref_available(const std::string& token_ref) const;
};

}  // namespace agentos

