#pragma once

#include "auth/auth_models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

struct AuthProfileMapping {
    AuthProviderId provider = AuthProviderId::openai;
    std::string profile_name = "default";
};

class AuthProfileStore {
public:
    explicit AuthProfileStore(std::filesystem::path store_path);

    void set_default(AuthProviderId provider, const std::string& profile_name);
    [[nodiscard]] std::optional<std::string> default_profile(AuthProviderId provider) const;
    [[nodiscard]] std::vector<AuthProfileMapping> list() const;
    void compact() const;
    [[nodiscard]] const std::filesystem::path& store_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<AuthProfileMapping> mappings_;
};

}  // namespace agentos
