#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/policy/approval_store.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/policy/permission_model.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/policy/role_catalog.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/local_planning_agent.hpp"
#include "memory/memory_manager.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"
#include "trust/allowlist_store.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_invite_store.hpp"
#include "trust/pairing_manager.hpp"
#include "trust/trust_policy.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_policy_trust_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

struct TestRuntime {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    agentos::Router router;
    agentos::PolicyEngine policy_engine;
    agentos::ExecutionCache execution_cache;
    agentos::AuditLogger audit_logger;
    agentos::MemoryManager memory_manager;
    agentos::AgentLoop loop;

    explicit TestRuntime(const std::filesystem::path& workspace)
        : execution_cache(workspace / "execution_cache.tsv"),
          audit_logger(workspace / "audit.log"),
          memory_manager(workspace / "memory"),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

struct TrustedTestRuntime {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    agentos::Router router;
    agentos::IdentityManager identity_manager;
    agentos::AllowlistStore allowlist_store;
    agentos::PairingManager pairing_manager;
    agentos::TrustPolicy trust_policy;
    agentos::PolicyEngine policy_engine;
    agentos::ExecutionCache execution_cache;
    agentos::AuditLogger audit_logger;
    agentos::MemoryManager memory_manager;
    agentos::AgentLoop loop;

    explicit TrustedTestRuntime(const std::filesystem::path& workspace)
        : identity_manager(workspace / "trust" / "identities.tsv"),
          allowlist_store(workspace / "trust" / "allowlist.tsv"),
          pairing_manager(allowlist_store),
          trust_policy(allowlist_store),
          policy_engine(agentos::PolicyEngineDependencies{.trust_policy = &trust_policy}),
          execution_cache(workspace / "trusted_execution_cache.tsv"),
          audit_logger(workspace / "trusted_audit.log"),
          memory_manager(workspace / "trusted_memory"),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void RegisterCore(TrustedTestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void TestPolicyRequiresIdempotencyKeyForSideEffects(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "idempotency-missing",
        .task_type = "write_file",
        .objective = "reject side-effecting write without idempotency",
        .workspace_path = workspace,
        .inputs = {
            {"path", "idempotency/missing.txt"},
            {"content", "hello"},
        },
    });

    Expect(!result.success, "side-effecting write without idempotency_key should be denied");
    Expect(result.error_code == "PolicyDenied", "missing idempotency_key should fail at policy layer");
    Expect(result.error_message.find("idempotency_key") != std::string::npos,
        "missing idempotency_key should return a clear policy reason");
}

void TestPolicyDeniesWorkspaceEscape(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "escape",
        .task_type = "read_file",
        .objective = "try to escape workspace",
        .workspace_path = workspace,
        .inputs = {
            {"path", "../outside.txt"},
        },
    });

    Expect(!result.success, "workspace escape should be denied");
    Expect(result.error_code == "PolicyDenied", "workspace escape should fail at policy layer");
}

void TestPolicyDeniesRemoteTaskWithoutTrustPolicy(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto decision = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "remote-without-trust-policy",
            .task_type = "read_file",
            .objective = "reject remote task without trust policy",
            .workspace_path = workspace,
            .remote_trigger = true,
            .origin_identity_id = "phone",
            .origin_device_id = "device-1",
        },
        agentos::SkillManifest{
            .name = "file_read",
            .version = "test",
            .description = "read test",
            .risk_level = "low",
            .permissions = {"filesystem.read"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "README.md"},
            },
        });

    Expect(!decision.allowed, "remote task should be denied when PolicyEngine has no TrustPolicy");
    Expect(decision.reason == "remote tasks require TrustPolicy",
        "remote task without TrustPolicy should return a clear denial reason");
}

void TestPermissionModelDeniesUnknownPermission(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto decision = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-permission",
            .task_type = "custom",
            .objective = "validate unknown permission handling",
            .workspace_path = workspace,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "low",
            .permissions = {"filesystem.read", "unknown.permission"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "README.md"},
            },
        });

    Expect(!decision.allowed, "unknown skill permission should be denied");
    Expect(decision.reason.find("unknown.permission") != std::string::npos, "unknown permission denial should name the permission");
}

void TestPermissionModelNamespaceWildcard(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto decision = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "wildcard-permission",
            .task_type = "custom",
            .objective = "validate namespace wildcard handling",
            .workspace_path = workspace,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "low",
            .permissions = {"filesystem.*"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "../outside.txt"},
            },
        });

    Expect(!decision.allowed, "filesystem.* permission should be treated as filesystem access");
    Expect(decision.reason == "path escapes the active workspace", "filesystem.* should trigger workspace boundary checks");
    Expect(agentos::PermissionModel::has_permission({"filesystem.*"}, agentos::PermissionNames::FilesystemWrite),
        "permission model should match namespace wildcard permissions");
    Expect(agentos::PermissionModel::unknown_permissions({"agent.dispatch"}).empty(),
        "agent.dispatch should be a known orchestration permission");
    Expect(agentos::PermissionModel::has_permission({"agent.*"}, agentos::PermissionNames::AgentDispatch),
        "agent.* should grant agent.dispatch");
}

void TestRoleCatalogGrantsUserPermissions(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "role_catalog_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::RoleCatalog role_catalog(isolated_workspace / "roles.tsv");
    role_catalog.save_role(agentos::RoleDefinition{
        .role_name = "reader",
        .permissions = {"filesystem.read"},
    });
    role_catalog.assign_user_roles(agentos::UserRoleAssignment{
        .user_id = "alice",
        .roles = {"reader"},
    });

    agentos::PolicyEngine policy_engine(agentos::PolicyEngineDependencies{.role_catalog = &role_catalog});
    const auto allowed = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "role-catalog-read",
            .task_type = "read_file",
            .objective = "role catalog allows filesystem read",
            .workspace_path = isolated_workspace,
            .user_id = "alice",
        },
        agentos::SkillManifest{
            .name = "file_read",
            .version = "test",
            .description = "read test",
            .risk_level = "low",
            .permissions = {"filesystem.read"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "README.md"},
            },
        });
    Expect(allowed.allowed, "role catalog should grant declared role permissions to a user");

    const auto denied = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "role-catalog-write",
            .task_type = "write_file",
            .objective = "role catalog denies missing write grant",
            .workspace_path = isolated_workspace,
            .user_id = "alice",
            .idempotency_key = "role-write",
        },
        agentos::SkillManifest{
            .name = "file_write",
            .version = "test",
            .description = "write test",
            .risk_level = "low",
            .permissions = {"filesystem.write"},
            .idempotent = false,
        },
        agentos::SkillCall{
            .idempotency_key = "role-write",
            .arguments = {
                {"path", "out.txt"},
            },
        });
    Expect(!denied.allowed, "role catalog should deny permissions not granted by the user's roles");
    Expect(denied.reason.find("filesystem.write") != std::string::npos,
        "role catalog denial should name the missing permission");

    agentos::RoleCatalog reloaded(isolated_workspace / "roles.tsv");
    const auto permissions = reloaded.permissions_for_user("alice");
    Expect(agentos::PermissionModel::has_permission(permissions, agentos::PermissionNames::FilesystemRead),
        "role catalog should persist user role permissions");

    Expect(reloaded.remove_role("reader"), "role catalog should remove existing roles");
    Expect(reloaded.permissions_for_user("alice").empty(),
        "removing a role should remove it from user assignments");
    agentos::RoleCatalog after_role_remove(isolated_workspace / "roles.tsv");
    Expect(after_role_remove.permissions_for_user("alice").empty(),
        "removed role cleanup should persist");

    after_role_remove.save_role(agentos::RoleDefinition{
        .role_name = "writer",
        .permissions = {"filesystem.write"},
    });
    after_role_remove.assign_user_roles(agentos::UserRoleAssignment{
        .user_id = "alice",
        .roles = {"writer"},
    });
    Expect(after_role_remove.remove_user_roles("alice"), "role catalog should remove user role assignments");
    Expect(after_role_remove.permissions_for_user("alice").empty(),
        "removed user role assignments should stop granting permissions");
}

void TestPermissionModelUnknownRiskRequiresApproval(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto denied = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-risk-denied",
            .task_type = "custom",
            .objective = "validate unknown risk handling",
            .workspace_path = workspace,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "experimental",
            .permissions = {},
        },
        agentos::SkillCall{});

    Expect(!denied.allowed, "unknown risk level should require high-risk approval");

    const auto missing_approval_id = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-risk-missing-approval-id",
            .task_type = "custom",
            .objective = "validate unknown risk approval hook",
            .workspace_path = workspace,
            .allow_high_risk = true,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "experimental",
            .permissions = {},
        },
        agentos::SkillCall{});

    Expect(!missing_approval_id.allowed, "allow_high_risk should still require approval_id for unknown risk level");
    Expect(missing_approval_id.reason.find("approval_id") != std::string::npos,
        "missing approval_id denial should name approval_id");

    const auto allowed = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "unknown-risk-allowed",
            .task_type = "custom",
            .objective = "validate unknown risk approval",
            .workspace_path = workspace,
            .allow_high_risk = true,
            .approval_id = "approval-smoke-1",
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom test skill",
            .risk_level = "experimental",
            .permissions = {},
        },
        agentos::SkillCall{});

    Expect(allowed.allowed, "allow_high_risk with approval_id should approve unknown risk level");
}

void TestPolicyRequiresApprovalIdForHighRiskAgents(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;
    const auto denied = policy_engine.evaluate_agent(
        agentos::TaskRequest{
            .task_id = "high-risk-agent-missing-approval",
            .task_type = "analysis",
            .objective = "validate high-risk agent approval",
            .workspace_path = workspace,
            .allow_high_risk = true,
        },
        agentos::AgentProfile{
            .agent_name = "high_risk_agent",
            .risk_level = "high",
        },
        agentos::AgentTask{
            .task_id = "high-risk-agent-missing-approval",
            .task_type = "analysis",
            .workspace_path = workspace.string(),
        });

    Expect(!denied.allowed, "high-risk agent should require approval_id when allow_high_risk is set");
    Expect(denied.reason.find("approval_id") != std::string::npos,
        "high-risk agent denial should name approval_id");

    const auto allowed = policy_engine.evaluate_agent(
        agentos::TaskRequest{
            .task_id = "high-risk-agent-approved",
            .task_type = "analysis",
            .objective = "validate approved high-risk agent",
            .workspace_path = workspace,
            .allow_high_risk = true,
            .approval_id = "approval-agent-1",
        },
        agentos::AgentProfile{
            .agent_name = "high_risk_agent",
            .risk_level = "high",
        },
        agentos::AgentTask{
            .task_id = "high-risk-agent-approved",
            .task_type = "analysis",
            .workspace_path = workspace.string(),
        });

    Expect(allowed.allowed, "high-risk agent should be allowed with approval_id");
}

void TestApprovalStoreConstrainsHighRiskPolicy(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "approval_store_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::ApprovalStore approval_store(isolated_workspace / "approvals.tsv");
    const auto requested = approval_store.request("critical skill", "test approval", "alice");
    Expect(!requested.approval_id.empty(), "approval request should generate an id");
    Expect(!approval_store.is_approved(requested.approval_id), "new approval should start pending");

    agentos::AllowlistStore allowlist_store(isolated_workspace / "allowlist.tsv");
    agentos::TrustPolicy trust_policy(allowlist_store);
    agentos::RoleCatalog role_catalog(isolated_workspace / "roles.tsv");
    agentos::PolicyEngine policy_engine(agentos::PolicyEngineDependencies{
        .trust_policy = &trust_policy,
        .role_catalog = &role_catalog,
        .approval_store = &approval_store,
    });

    const auto pending_denied = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "high-risk-pending-approval",
            .task_type = "custom",
            .objective = "validate pending approval denial",
            .workspace_path = isolated_workspace,
            .allow_high_risk = true,
            .approval_id = requested.approval_id,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom high risk",
            .risk_level = "high",
        },
        agentos::SkillCall{});
    Expect(!pending_denied.allowed, "pending approval should not authorize high-risk execution");
    Expect(pending_denied.reason.find("not approved") != std::string::npos,
        "pending approval denial should name approval state");

    Expect(approval_store.approve(requested.approval_id, "admin"),
        "approval store should approve existing requests");
    agentos::ApprovalStore reloaded(isolated_workspace / "approvals.tsv");
    Expect(reloaded.is_approved(requested.approval_id), "approved request should persist");

    agentos::PolicyEngine approved_policy(agentos::PolicyEngineDependencies{
        .trust_policy = &trust_policy,
        .role_catalog = &role_catalog,
        .approval_store = &reloaded,
    });
    const auto allowed = approved_policy.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "high-risk-approved-by-store",
            .task_type = "custom",
            .objective = "validate approved high-risk",
            .workspace_path = isolated_workspace,
            .allow_high_risk = true,
            .approval_id = requested.approval_id,
        },
        agentos::SkillManifest{
            .name = "custom",
            .version = "test",
            .description = "custom high risk",
            .risk_level = "high",
        },
        agentos::SkillCall{});
    Expect(allowed.allowed, "approved persisted approval should authorize high-risk execution");

    Expect(reloaded.revoke(requested.approval_id, "admin"), "approval store should revoke existing approvals");
    Expect(!reloaded.is_approved(requested.approval_id), "revoked approval should stop authorizing execution");
}

void TestPolicyPermissionGrantsConstrainSkillsAndAgents(const std::filesystem::path& workspace) {
    agentos::PolicyEngine policy_engine;

    const auto denied_skill = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "grant-denied-skill",
            .task_type = "write_file",
            .objective = "validate denied skill grant",
            .workspace_path = workspace,
            .permission_grants = {"filesystem.read"},
        },
        agentos::SkillManifest{
            .name = "file_write",
            .version = "test",
            .description = "write test",
            .risk_level = "medium",
            .permissions = {"filesystem.write"},
        },
        agentos::SkillCall{
            .arguments = {
                {"path", "grant-denied.txt"},
            },
        });

    Expect(!denied_skill.allowed, "skill should be denied when task grants do not include required permission");
    Expect(denied_skill.reason.find("filesystem.write") != std::string::npos,
        "skill grant denial should name missing permission");

    const auto allowed_skill = policy_engine.evaluate_skill(
        agentos::TaskRequest{
            .task_id = "grant-allowed-skill",
            .task_type = "write_file",
            .objective = "validate allowed skill grant",
            .workspace_path = workspace,
            .idempotency_key = "grant-allowed-skill",
            .permission_grants = {"filesystem.*"},
        },
        agentos::SkillManifest{
            .name = "file_write",
            .version = "test",
            .description = "write test",
            .risk_level = "medium",
            .permissions = {"filesystem.write"},
        },
        agentos::SkillCall{
            .idempotency_key = "grant-allowed-skill",
            .arguments = {
                {"path", "grant-allowed.txt"},
            },
        });

    Expect(allowed_skill.allowed, "skill should be allowed when task grants include required permission");

    const auto denied_agent = policy_engine.evaluate_agent(
        agentos::TaskRequest{
            .task_id = "grant-denied-agent",
            .task_type = "analysis",
            .objective = "validate denied agent grant",
            .workspace_path = workspace,
            .permission_grants = {"filesystem.read"},
        },
        agentos::AgentProfile{
            .agent_name = "planner",
            .risk_level = "low",
        },
        agentos::AgentTask{
            .task_id = "grant-denied-agent",
            .task_type = "analysis",
            .workspace_path = workspace.string(),
        });

    Expect(!denied_agent.allowed, "agent should be denied without agent.invoke grant");
    Expect(denied_agent.reason.find("agent.invoke") != std::string::npos,
        "agent grant denial should name agent.invoke");

    const auto allowed_agent = policy_engine.evaluate_agent(
        agentos::TaskRequest{
            .task_id = "grant-allowed-agent",
            .task_type = "analysis",
            .objective = "validate allowed agent grant",
            .workspace_path = workspace,
            .permission_grants = {"agent.invoke"},
        },
        agentos::AgentProfile{
            .agent_name = "planner",
            .risk_level = "low",
        },
        agentos::AgentTask{
            .task_id = "grant-allowed-agent",
            .task_type = "analysis",
            .workspace_path = workspace.string(),
        });

    Expect(allowed_agent.allowed, "agent should be allowed with agent.invoke grant");
}

void TestRemoteTaskRequiresPairing(const std::filesystem::path& workspace) {
    TrustedTestRuntime runtime(workspace);
    RegisterCore(runtime);

    const auto denied = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-denied",
        .task_type = "read_file",
        .objective = "remote read without pairing",
        .workspace_path = workspace,
        .remote_trigger = true,
        .origin_identity_id = "phone",
        .origin_device_id = "device-1",
        .inputs = {
            {"path", "missing.txt"},
        },
    });

    Expect(!denied.success, "unpaired remote task should be denied");
    Expect(denied.error_code == "PolicyDenied", "unpaired remote task should fail at policy layer");

    runtime.identity_manager.ensure("phone", "local-user", "test phone");
    runtime.pairing_manager.pair("phone", "device-1", "test phone", {"task.submit"});

    const auto write = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-allowed-write",
        .task_type = "write_file",
        .objective = "create remote-readable file",
        .workspace_path = workspace,
        .idempotency_key = "remote-allowed-write",
        .inputs = {
            {"path", "remote/allowed.txt"},
            {"content", "ok"},
        },
    });
    Expect(write.success, "local write should prepare remote read file");

    const auto allowed = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-allowed",
        .task_type = "read_file",
        .objective = "remote read after pairing",
        .workspace_path = workspace,
        .remote_trigger = true,
        .origin_identity_id = "phone",
        .origin_device_id = "device-1",
        .inputs = {
            {"path", "remote/allowed.txt"},
        },
    });

    Expect(allowed.success, "paired remote task should be allowed");
    Expect(std::filesystem::exists(workspace / "trust" / "identities.tsv"), "identity store should be persisted");
    Expect(std::filesystem::exists(workspace / "trust" / "allowlist.tsv"), "allowlist should be persisted");
}

void TestBlockedRemoteTaskIsDenied(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "remote_blocked_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TrustedTestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    runtime.identity_manager.ensure("phone", "local-user", "blocked phone");
    runtime.pairing_manager.pair("phone", "device-1", "blocked phone", {"task.submit"});
    runtime.pairing_manager.block("phone", "device-1");

    const auto result = runtime.loop.run(agentos::TaskRequest{
        .task_id = "remote-blocked",
        .task_type = "read_file",
        .objective = "remote read after block",
        .workspace_path = isolated_workspace,
        .remote_trigger = true,
        .origin_identity_id = "phone",
        .origin_device_id = "device-1",
        .inputs = {
            {"path", "remote/blocked.txt"},
        },
    });

    Expect(!result.success, "blocked remote task should be denied");
    Expect(result.error_code == "PolicyDenied", "blocked remote task should fail at policy layer");
    Expect(result.error_message.find("blocked") != std::string::npos,
        "blocked remote task should include the blocked-state reason");
}

void TestPairingManagerDeviceLifecyclePersists(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "device_lifecycle_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::AllowlistStore allowlist_store(isolated_workspace / "allowlist.tsv");
    agentos::PairingManager pairing_manager(allowlist_store);
    const auto paired = pairing_manager.pair("phone", "device-1", "dev phone", {"task.submit"});
    Expect(paired.paired_epoch_ms > 0, "paired device should record paired timestamp");
    Expect(paired.last_seen_epoch_ms > 0, "paired device should record last_seen timestamp");

    Expect(pairing_manager.rename_device("phone", "device-1", "renamed phone"),
        "device lifecycle should rename paired devices");
    Expect(pairing_manager.mark_seen("phone", "device-1"), "device lifecycle should update last_seen");
    pairing_manager.block("phone", "device-1");
    Expect(pairing_manager.unblock("phone", "device-1"), "device lifecycle should unblock existing devices");

    agentos::AllowlistStore reloaded(isolated_workspace / "allowlist.tsv");
    const auto peer = reloaded.find("phone", "device-1");
    Expect(peer.has_value(), "device lifecycle changes should persist");
    if (peer.has_value()) {
        Expect(peer->label == "renamed phone", "renamed device label should persist");
        Expect(peer->trust_level == agentos::TrustLevel::paired, "unblocked device should persist as paired");
        Expect(peer->paired_epoch_ms > 0, "paired timestamp should persist");
        Expect(peer->last_seen_epoch_ms > 0, "last_seen timestamp should persist");
    }

    Expect(!pairing_manager.rename_device("phone", "missing", "missing"),
        "device lifecycle rename should report missing devices");
    Expect(!pairing_manager.mark_seen("phone", "missing"),
        "device lifecycle seen update should report missing devices");
    Expect(!pairing_manager.unblock("phone", "missing"),
        "device lifecycle unblock should report missing devices");
}

void TestPairingInviteStoreCreatesAndConsumes(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "pairing_invite_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::PairingInviteStore invites(isolated_workspace / "invites.tsv");
    const auto invite = invites.create(
        "phone",
        "device-1",
        "dev phone",
        "local-user",
        "phone identity",
        {"task.submit"},
        600);

    Expect(!invite.token.empty(), "pairing invite should generate a token");
    Expect(invites.list_active().size() == 1, "pairing invite should be listed while active");

    agentos::PairingInviteStore reloaded(isolated_workspace / "invites.tsv");
    const auto consumed = reloaded.consume(invite.token);
    Expect(consumed.has_value(), "pairing invite should survive reload and be consumable");
    if (consumed.has_value()) {
        Expect(consumed->identity_id == "phone", "consumed invite should keep identity");
        Expect(consumed->device_id == "device-1", "consumed invite should keep device");
        Expect(consumed->permissions == std::vector<std::string>{"task.submit"},
            "consumed invite should keep permissions");
    }
    Expect(!reloaded.consume(invite.token).has_value(), "pairing invite should be single-use");

    bool rejected_unknown_permission = false;
    try {
        reloaded.create("phone", "device-2", "", "", "", {"unknown.permission"}, 600);
    } catch (const std::exception&) {
        rejected_unknown_permission = true;
    }
    Expect(rejected_unknown_permission, "pairing invite should reject unknown permissions");

    bool rejected_bad_ttl = false;
    try {
        reloaded.create("phone", "device-2", "", "", "", {"task.submit"}, 0);
    } catch (const std::exception&) {
        rejected_bad_ttl = true;
    }
    Expect(rejected_bad_ttl, "pairing invite should reject non-positive ttl");
}

void TestIdentityManagerPersistsAndUpdates(const std::filesystem::path& workspace) {
    const auto identity_store = workspace / "trust_identity" / "identities.tsv";

    agentos::IdentityManager manager(identity_store);
    const auto saved = manager.save(agentos::Identity{
        .identity_id = "phone",
        .user_id = "user-a",
        .label = "dev phone",
    });

    Expect(saved.identity_id == "phone", "identity manager should save identity");
    Expect(manager.find("phone").has_value(), "identity manager should find saved identity");

    manager.save(agentos::Identity{
        .identity_id = "phone",
        .user_id = "user-a",
        .label = "renamed phone",
    });

    agentos::IdentityManager reloaded(identity_store);
    const auto reloaded_identity = reloaded.find("phone");
    Expect(reloaded_identity.has_value(), "identity manager should reload persisted identity");
    if (reloaded_identity.has_value()) {
        Expect(reloaded_identity->label == "renamed phone", "identity manager should update existing identity");
    }

    Expect(reloaded.remove("phone"), "identity manager should remove identity");
    Expect(!reloaded.find("phone").has_value(), "identity manager should not find removed identity");
}

void TestTrustAuditEvent(const std::filesystem::path& workspace) {
    const auto audit_path = workspace / "trust_mutation_audit.log";
    agentos::AuditLogger audit_logger(audit_path);

    audit_logger.record_trust_event("pair", "phone", "device-1", true, "peer paired");

    std::ifstream input(audit_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto audit_text = buffer.str();

    Expect(audit_text.find(R"("event":"trust")") != std::string::npos, "trust audit event should be recorded");
    Expect(audit_text.find(R"("action":"pair")") != std::string::npos, "trust audit action should be recorded");
    Expect(audit_text.find(R"("identity_id":"phone")") != std::string::npos, "trust audit identity should be recorded");
    Expect(audit_text.find(R"("device_id":"device-1")") != std::string::npos, "trust audit device should be recorded");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestPolicyRequiresIdempotencyKeyForSideEffects(workspace);
    TestPolicyDeniesWorkspaceEscape(workspace);
    TestPolicyDeniesRemoteTaskWithoutTrustPolicy(workspace);
    TestPermissionModelDeniesUnknownPermission(workspace);
    TestPermissionModelNamespaceWildcard(workspace);
    TestRoleCatalogGrantsUserPermissions(workspace);
    TestPermissionModelUnknownRiskRequiresApproval(workspace);
    TestPolicyRequiresApprovalIdForHighRiskAgents(workspace);
    TestApprovalStoreConstrainsHighRiskPolicy(workspace);
    TestPolicyPermissionGrantsConstrainSkillsAndAgents(workspace);
    TestRemoteTaskRequiresPairing(workspace);
    TestBlockedRemoteTaskIsDenied(workspace);
    TestPairingManagerDeviceLifecyclePersists(workspace);
    TestPairingInviteStoreCreatesAndConsumes(workspace);
    TestIdentityManagerPersistsAndUpdates(workspace);
    TestTrustAuditEvent(workspace);

    if (failures != 0) {
        std::cerr << failures << " policy/trust test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_policy_trust_tests passed\n";
    return 0;
}
