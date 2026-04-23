#pragma once

#include "trust/trust_models.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace agentos {

class IdentityManager {
public:
    explicit IdentityManager(std::filesystem::path store_path);

    Identity save(Identity identity);
    Identity ensure(const std::string& identity_id, const std::string& user_id, const std::string& label);
    bool remove(const std::string& identity_id);
    [[nodiscard]] std::optional<Identity> find(const std::string& identity_id) const;
    [[nodiscard]] std::vector<Identity> list() const;

    [[nodiscard]] const std::filesystem::path& store_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<Identity> identities_;
};

}  // namespace agentos
