#pragma once

#include "trust/trust_models.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace agentos {

class AllowlistStore {
public:
    explicit AllowlistStore(std::filesystem::path store_path);

    void save(const TrustedPeer& peer);
    std::optional<TrustedPeer> find(const std::string& identity_id, const std::string& device_id) const;
    std::vector<TrustedPeer> list() const;
    void remove(const std::string& identity_id, const std::string& device_id);

    [[nodiscard]] const std::filesystem::path& store_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<TrustedPeer> peers_;
};

}  // namespace agentos

