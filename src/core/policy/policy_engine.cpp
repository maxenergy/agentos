#include "core/policy/policy_engine.hpp"

#include "core/policy/permission_model.hpp"
#include "utils/path_utils.hpp"

#include <sstream>

namespace agentos {

PolicyEngine::PolicyEngine(PolicyEngineDependencies dependencies)
    : dependencies_(dependencies) {}

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

bool RequiresIdempotencyKey(const SkillManifest& manifest) {
    return !manifest.idempotent &&
           PermissionModel::has_permission(manifest.permissions, PermissionNames::FilesystemWrite);
}

PolicyDecision EvaluateHighRiskApproval(
    const bool requires_approval,
    const bool allow_high_risk,
    const std::string& approval_id,
    const ApprovalStore* approval_store,
    const std::string& target_kind) {
    if (!requires_approval) {
        return {true, "approval not required"};
    }

    if (!allow_high_risk) {
        return {false, "approval required: high-risk " + target_kind + " require allow_high_risk=true"};
    }

    if (approval_id.empty()) {
        return {false, "approval required: high-risk " + target_kind + " require approval_id"};
    }

    if (approval_store && !approval_store->is_approved(approval_id)) {
        return {false, "approval required: approval_id is not approved"};
    }

    return {true, "approved by " + approval_id};
}

PolicyDecision EvaluatePermissionGrants(
    const std::vector<std::string>& grants,
    const std::vector<std::string>& required_permissions,
    const std::string& target_name) {
    if (grants.empty()) {
        return {true, "permission grants not constrained"};
    }

    for (const auto& permission : required_permissions) {
        if (!PermissionModel::has_permission(grants, permission)) {
            return {false, "permission grant missing for " + target_name + ": " + permission};
        }
    }

    return {true, "permission grants satisfied"};
}

}  // namespace

PolicyDecision PolicyEngine::evaluate_task_origin(const TaskRequest& task) const {
    if (dependencies_.trust_policy) {
        return dependencies_.trust_policy->evaluate_task_origin(task);
    }

    if (task.remote_trigger) {
        return {false, "remote tasks require TrustPolicy"};
    }

    return {true, "local task origin"};
}

std::vector<std::string> PolicyEngine::effective_permission_grants(const TaskRequest& task) const {
    if (!task.permission_grants.empty()) {
        return task.permission_grants;
    }
    if (!dependencies_.role_catalog) {
        return {};
    }
    return dependencies_.role_catalog->permissions_for_user(task.user_id);
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

    if (const auto grants = EvaluatePermissionGrants(effective_permission_grants(task), manifest.permissions, manifest.name);
        !grants.allowed) {
        return grants;
    }

    if (const auto approval = EvaluateHighRiskApproval(
            PermissionModel::requires_high_risk_approval(manifest.risk_level),
            task.allow_high_risk,
            task.approval_id,
            dependencies_.approval_store,
            "skills");
        !approval.allowed) {
        return approval;
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

    if (RequiresIdempotencyKey(manifest) && call.idempotency_key.empty()) {
        return {false, "side-effecting skills require idempotency_key"};
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

    if (const auto grants = EvaluatePermissionGrants(
            effective_permission_grants(task),
            {std::string(PermissionNames::AgentInvoke)},
            profile.agent_name);
        !grants.allowed) {
        return grants;
    }

    if (const auto approval = EvaluateHighRiskApproval(
            PermissionModel::requires_high_risk_approval(profile.risk_level),
            task.allow_high_risk,
            task.approval_id,
            dependencies_.approval_store,
            "agents");
        !approval.allowed) {
        return approval;
    }

    if (task.workspace_path.empty()) {
        return {false, "workspace_path is required for agent execution"};
    }

    return {true, "allowed"};
}

}  // namespace agentos
