#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace agentos {

struct RoleDefinition {
    std::string role_name;
    std::vector<std::string> permissions;
};

struct UserRoleAssignment {
    std::string user_id;
    std::vector<std::string> roles;
};

class RoleCatalog {
public:
    explicit RoleCatalog(std::filesystem::path store_path);

    void save_role(RoleDefinition role);
    void assign_user_roles(UserRoleAssignment assignment);
    bool remove_role(const std::string& role_name);
    bool remove_user_roles(const std::string& user_id);
    std::vector<RoleDefinition> list_roles() const;
    std::vector<UserRoleAssignment> list_user_roles() const;
    std::vector<std::string> permissions_for_user(const std::string& user_id) const;

    [[nodiscard]] const std::filesystem::path& store_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::map<std::string, RoleDefinition> roles_;
    std::map<std::string, UserRoleAssignment> user_roles_;
};

}  // namespace agentos
