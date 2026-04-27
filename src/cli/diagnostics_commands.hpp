#pragma once

#include "auth/auth_manager.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "core/policy/approval_store.hpp"
#include "core/policy/role_catalog.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "hosts/plugin/plugin_host.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/storage_version_store.hpp"
#include "trust/allowlist_store.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_invite_store.hpp"

#include <filesystem>

namespace agentos {

// Read-only one-shot snapshot of trivially-accessible runtime state, intended
// for support workflows.  Never mutates any store.  Each section is best-effort:
// a thrown exception inside one section is captured and reported, but does not
// abort the rest of the snapshot.
int RunDiagnosticsCommand(
    const std::filesystem::path& workspace,
    const SkillRegistry& skill_registry,
    const AgentRegistry& agent_registry,
    AuthManager& auth_manager,
    const SessionStore& session_store,
    const AuthProfileStore& auth_profile_store,
    const SecureTokenStore& token_store,
    const PluginHost& plugin_host,
    const Scheduler& scheduler,
    const StorageVersionStore& storage_version_store,
    const IdentityManager& identity_manager,
    const AllowlistStore& allowlist_store,
    const PairingInviteStore& pairing_invite_store,
    const RoleCatalog& role_catalog,
    const ApprovalStore& approval_store,
    int argc,
    char* argv[]);

}  // namespace agentos
