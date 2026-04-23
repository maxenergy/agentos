#include "trust/trust_policy.hpp"

#include "core/policy/permission_model.hpp"

namespace agentos {

namespace {

bool HasPermission(const TrustedPeer& peer, const std::string& permission) {
    return PermissionModel::has_permission(peer.permissions, permission);
}

}  // namespace

TrustPolicy::TrustPolicy(const AllowlistStore& allowlist_store)
    : allowlist_store_(allowlist_store) {}

PolicyDecision TrustPolicy::evaluate_task_origin(const TaskRequest& task) const {
    if (!task.remote_trigger) {
        return {true, "local task origin"};
    }

    if (task.origin_identity_id.empty() || task.origin_device_id.empty()) {
        return {false, "remote tasks require origin_identity_id and origin_device_id"};
    }

    const auto peer = allowlist_store_.find(task.origin_identity_id, task.origin_device_id);
    if (!peer.has_value()) {
        return {false, "remote origin is not paired"};
    }

    if (peer->trust_level == TrustLevel::blocked) {
        return {false, "remote origin is blocked"};
    }

    if (!HasPermission(*peer, std::string(PermissionNames::TaskSubmit))) {
        return {false, "remote origin lacks task.submit permission"};
    }

    return {true, "paired remote origin"};
}

}  // namespace agentos
