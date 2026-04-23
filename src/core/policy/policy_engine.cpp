#include "core/policy/policy_engine.hpp"

#include "core/policy/permission_model.hpp"
#include "utils/path_utils.hpp"

#include <sstream>

namespace agentos {

PolicyEngine::PolicyEngine(const TrustPolicy& trust_policy)
    : trust_policy_(&trust_policy) {}

namespace {

std::string JoinUnknownPermissions(const std::vector<std::string>& permissions) {
    std::ostringstream output;
    for (std::size_t index = 0; index < permissions.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << permissions[index];
    }
    return output.str();
}

}  // namespace

PolicyDecision PolicyEngine::evaluate_task_origin(const TaskRequest& task) const {
    if (trust_policy_) {
        return trust_policy_->evaluate_task_origin(task);
    }

    if (task.remote_trigger) {
        return {false, "remote tasks require TrustPolicy"};
    }

    return {true, "local task origin"};
}

PolicyDecision PolicyEngine::evaluate_skill(
    const TaskRequest& task,
    const SkillManifest& manifest,
    const SkillCall& call) const {
    if (const auto origin = evaluate_task_origin(task); !origin.allowed) {
        return origin;
    }

    const auto unknown_permissions = PermissionModel::unknown_permissions(manifest.permissions);
    if (!unknown_permissions.empty()) {
        return {false, "unknown permissions: " + JoinUnknownPermissions(unknown_permissions)};
    }

    if (PermissionModel::requires_high_risk_approval(manifest.risk_level) && !task.allow_high_risk) {
        return {false, "high-risk skills require allow_high_risk=true"};
    }

    if (PermissionModel::has_permission(manifest.permissions, PermissionNames::NetworkAccess) && !task.allow_network) {
        return {false, "network access is disabled for this task"};
    }

    const bool touches_filesystem =
        PermissionModel::has_permission(manifest.permissions, PermissionNames::FilesystemRead) ||
        PermissionModel::has_permission(manifest.permissions, PermissionNames::FilesystemWrite);

    if (touches_filesystem) {
        const auto path = call.get_arg("path").value_or(".");
        const auto resolved_path = ResolveWorkspacePath(task.workspace_path, path);
        if (!IsPathInsideWorkspace(task.workspace_path, resolved_path)) {
            return {false, "path escapes the active workspace"};
        }
    }

    return {true, "allowed"};
}

PolicyDecision PolicyEngine::evaluate_agent(
    const TaskRequest& task,
    const AgentProfile& profile,
    const AgentTask& agent_task) const {
    (void)agent_task;

    if (const auto origin = evaluate_task_origin(task); !origin.allowed) {
        return origin;
    }

    if (PermissionModel::requires_high_risk_approval(profile.risk_level) && !task.allow_high_risk) {
        return {false, "high-risk agents require allow_high_risk=true"};
    }

    if (task.workspace_path.empty()) {
        return {false, "workspace_path is required for agent execution"};
    }

    return {true, "allowed"};
}

}  // namespace agentos
