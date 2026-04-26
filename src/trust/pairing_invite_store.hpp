#pragma once

#include "trust/trust_models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

class PairingInviteStore {
public:
    explicit PairingInviteStore(std::filesystem::path store_path);

    PairingInvite create(
        const std::string& identity_id,
        const std::string& device_id,
        const std::string& label,
        const std::string& user_id,
        const std::string& identity_label,
        std::vector<std::string> permissions,
        int ttl_seconds);

    std::optional<PairingInvite> consume(const std::string& token);
    std::vector<PairingInvite> list_active() const;

    [[nodiscard]] const std::filesystem::path& store_path() const;

    static long long NowEpochMs();

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<PairingInvite> invites_;
};

}  // namespace agentos
