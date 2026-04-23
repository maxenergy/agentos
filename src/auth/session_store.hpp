#pragma once

#include "auth/auth_models.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace agentos {

class SessionStore {
public:
    explicit SessionStore(std::filesystem::path store_path);

    void save(const AuthSession& session);
    std::optional<AuthSession> find(AuthProviderId provider, const std::string& profile_name) const;
    std::vector<AuthSession> list() const;
    void remove(AuthProviderId provider, const std::string& profile_name);

    [[nodiscard]] const std::filesystem::path& store_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<AuthSession> sessions_;
};

}  // namespace agentos

