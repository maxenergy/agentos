#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agentos {

namespace PermissionNames {

inline constexpr std::string_view All = "*";
inline constexpr std::string_view FilesystemRead = "filesystem.read";
inline constexpr std::string_view FilesystemWrite = "filesystem.write";
inline constexpr std::string_view ProcessSpawn = "process.spawn";
inline constexpr std::string_view NetworkAccess = "network.access";
inline constexpr std::string_view AgentInvoke = "agent.invoke";
inline constexpr std::string_view AgentDispatch = "agent.dispatch";
inline constexpr std::string_view TaskSubmit = "task.submit";

}  // namespace PermissionNames

enum class RiskLevel {
    low,
    medium,
    high,
    critical,
    unknown,
};

class PermissionModel {
public:
    static RiskLevel parse_risk_level(const std::string& risk_level);
    static bool requires_high_risk_approval(const std::string& risk_level);
    static bool has_permission(const std::vector<std::string>& permissions, std::string_view permission);
    static std::vector<std::string> unknown_permissions(const std::vector<std::string>& permissions);

private:
    static bool is_known_permission(std::string_view permission);
    static bool permission_matches(std::string_view granted, std::string_view requested);
};

}  // namespace agentos
