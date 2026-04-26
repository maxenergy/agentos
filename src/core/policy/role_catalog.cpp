#include "core/policy/role_catalog.hpp"

#include "core/policy/permission_model.hpp"
#include "utils/atomic_file.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

void ValidatePermissions(const std::vector<std::string>& permissions) {
    if (const auto unknown = PermissionModel::unknown_permissions(permissions); !unknown.empty()) {
        throw std::invalid_argument("unknown role permissions: " + JoinStrings(unknown));
    }
}

std::string SerializeList(const std::vector<std::string>& values) {
    return JoinStrings(values);
}

std::vector<std::string> DeserializeList(const std::string& value) {
    return SplitNonEmpty(value, ',');
}

}  // namespace

RoleCatalog::RoleCatalog(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

void RoleCatalog::save_role(RoleDefinition role) {
    if (role.role_name.empty()) {
        throw std::invalid_argument("role name is required");
    }
    ValidatePermissions(role.permissions);
    roles_[role.role_name] = std::move(role);
    flush();
}

void RoleCatalog::assign_user_roles(UserRoleAssignment assignment) {
    if (assignment.user_id.empty()) {
        throw std::invalid_argument("user_id is required");
    }
    for (const auto& role : assignment.roles) {
        if (role.empty() || !roles_.contains(role)) {
            throw std::invalid_argument("unknown role assignment: " + role);
        }
    }
    user_roles_[assignment.user_id] = std::move(assignment);
    flush();
}

bool RoleCatalog::remove_role(const std::string& role_name) {
    const auto removed = roles_.erase(role_name) > 0;
    if (!removed) {
        return false;
    }

    for (auto it = user_roles_.begin(); it != user_roles_.end();) {
        auto& roles = it->second.roles;
        roles.erase(std::remove(roles.begin(), roles.end(), role_name), roles.end());
        if (roles.empty()) {
            it = user_roles_.erase(it);
        } else {
            ++it;
        }
    }
    flush();
    return true;
}

bool RoleCatalog::remove_user_roles(const std::string& user_id) {
    const auto removed = user_roles_.erase(user_id) > 0;
    if (removed) {
        flush();
    }
    return removed;
}

std::vector<RoleDefinition> RoleCatalog::list_roles() const {
    std::vector<RoleDefinition> roles;
    roles.reserve(roles_.size());
    for (const auto& [unused_name, role] : roles_) {
        (void)unused_name;
        roles.push_back(role);
    }
    return roles;
}

std::vector<UserRoleAssignment> RoleCatalog::list_user_roles() const {
    std::vector<UserRoleAssignment> assignments;
    assignments.reserve(user_roles_.size());
    for (const auto& [unused_user, assignment] : user_roles_) {
        (void)unused_user;
        assignments.push_back(assignment);
    }
    return assignments;
}

std::vector<std::string> RoleCatalog::permissions_for_user(const std::string& user_id) const {
    std::vector<std::string> permissions;
    const auto assignment = user_roles_.find(user_id);
    if (assignment == user_roles_.end()) {
        return permissions;
    }

    for (const auto& role_name : assignment->second.roles) {
        const auto role = roles_.find(role_name);
        if (role == roles_.end()) {
            continue;
        }
        for (const auto& permission : role->second.permissions) {
            if (std::find(permissions.begin(), permissions.end(), permission) == permissions.end()) {
                permissions.push_back(permission);
            }
        }
    }
    return permissions;
}

const std::filesystem::path& RoleCatalog::store_path() const {
    return store_path_;
}

void RoleCatalog::load() {
    roles_.clear();
    user_roles_.clear();

    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitTsvFields(line);
        if (parts.size() < 3) {
            continue;
        }
        if (parts[0] == "role") {
            RoleDefinition role{
                .role_name = parts[1],
                .permissions = DeserializeList(parts[2]),
            };
            if (role.role_name.empty() || !PermissionModel::unknown_permissions(role.permissions).empty()) {
                continue;
            }
            roles_[role.role_name] = std::move(role);
        } else if (parts[0] == "user") {
            UserRoleAssignment assignment{
                .user_id = parts[1],
                .roles = DeserializeList(parts[2]),
            };
            if (!assignment.user_id.empty()) {
                user_roles_[assignment.user_id] = std::move(assignment);
            }
        }
    }
}

void RoleCatalog::flush() const {
    std::ostringstream output;
    for (const auto& [unused_name, role] : roles_) {
        (void)unused_name;
        output << "role" << kDelimiter << role.role_name << kDelimiter << SerializeList(role.permissions) << '\n';
    }
    for (const auto& [unused_user, assignment] : user_roles_) {
        (void)unused_user;
        output << "user" << kDelimiter << assignment.user_id << kDelimiter << SerializeList(assignment.roles) << '\n';
    }
    WriteFileAtomically(store_path_, output.str());
}

}  // namespace agentos
