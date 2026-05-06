#include "core/policy/permission_model.hpp"

#include <algorithm>
#include <array>

namespace agentos {

namespace {

bool EndsWith(const std::string_view value, const std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool StartsWith(const std::string_view value, const std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

constexpr std::array<std::string_view, 7> kKnownPermissions{
    PermissionNames::FilesystemRead,
    PermissionNames::FilesystemWrite,
    PermissionNames::ProcessSpawn,
    PermissionNames::NetworkAccess,
    PermissionNames::AgentInvoke,
    PermissionNames::AgentDispatch,
    PermissionNames::TaskSubmit,
};

constexpr std::array<std::string_view, 5> kKnownNamespaces{
    "filesystem.",
    "process.",
    "network.",
    "agent.",
    "task.",
};

}  // namespace

RiskLevel PermissionModel::parse_risk_level(const std::string& risk_level) {
    if (risk_level == "low") {
        return RiskLevel::low;
    }
    if (risk_level == "medium") {
        return RiskLevel::medium;
    }
    if (risk_level == "high") {
        return RiskLevel::high;
    }
    if (risk_level == "critical") {
        return RiskLevel::critical;
    }
    return RiskLevel::unknown;
}

bool PermissionModel::requires_high_risk_approval(const std::string& risk_level) {
    switch (parse_risk_level(risk_level)) {
    case RiskLevel::high:
    case RiskLevel::critical:
    case RiskLevel::unknown:
        return true;
    case RiskLevel::low:
    case RiskLevel::medium:
    default:
        return false;
    }
}

bool PermissionModel::has_permission(
    const std::vector<std::string>& permissions,
    const std::string_view permission) {
    return std::any_of(permissions.begin(), permissions.end(), [&](const std::string& granted) {
        return permission_matches(granted, permission);
    });
}

std::vector<std::string> PermissionModel::unknown_permissions(const std::vector<std::string>& permissions) {
    std::vector<std::string> unknown;
    for (const auto& permission : permissions) {
        if (!is_known_permission(permission)) {
            unknown.push_back(permission);
        }
    }
    return unknown;
}

bool PermissionModel::is_known_permission(const std::string_view permission) {
    if (permission == PermissionNames::All) {
        return true;
    }

    if (std::find(kKnownPermissions.begin(), kKnownPermissions.end(), permission) != kKnownPermissions.end()) {
        return true;
    }

    if (EndsWith(permission, ".*")) {
        const auto prefix = permission.substr(0, permission.size() - 1);
        return std::find(kKnownNamespaces.begin(), kKnownNamespaces.end(), prefix) != kKnownNamespaces.end();
    }

    return false;
}

bool PermissionModel::permission_matches(
    const std::string_view granted,
    const std::string_view requested) {
    if (granted == PermissionNames::All || granted == requested) {
        return true;
    }

    if (!EndsWith(granted, ".*")) {
        return false;
    }

    const auto prefix = granted.substr(0, granted.size() - 1);
    return StartsWith(requested, prefix);
}

}  // namespace agentos
