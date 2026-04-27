#include "cli/diagnostics_commands.hpp"

#include "hosts/cli/cli_spec_loader.hpp"
#include "storage/storage_policy.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

// Build version string from CMake's project version.  CMake passes
// PROJECT_VERSION to add_definitions implicitly only if we set it; here we
// just hardcode the same string the CMakeLists.txt project() declaration
// uses (kept in sync with the top-level project version).
constexpr const char* kAgentOsVersion = "0.1.0";

constexpr const char* kPlatformName =
#if defined(_WIN32)
    "Windows";
#else
    "POSIX";
#endif

std::map<std::string, std::string> ParseOptions(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        const std::string arg = argv[index];
        const auto delimiter = arg.find('=');
        if (delimiter == std::string::npos) {
            options[arg] = "true";
        } else {
            options[arg.substr(0, delimiter)] = arg.substr(delimiter + 1);
        }
    }
    return options;
}

std::string JoinStrings(const std::vector<std::string>& values, const std::string& delimiter = ",") {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            output << delimiter;
        }
        output << values[index];
    }
    return output.str();
}

// ---------------------------------------------------------------------------
// Section data containers.  Each section runs in its own try/catch and stores
// either populated data plus a `populated=true` flag, or an `error` string.
// ---------------------------------------------------------------------------

struct PlatformSection {
    std::string os_name;
    std::string workspace;
    std::string error;
    bool populated = false;
};

struct BuildSection {
    std::string version;
    std::string error;
    bool populated = false;
};

struct AuthProviderRow {
    std::string provider;
    std::string default_profile;
    int session_count = 0;
};

struct AuthSection {
    std::vector<AuthProviderRow> providers;
    std::string credential_backend;
    bool credential_keychain_backed = false;
    std::string credential_message;
    std::string error;
    bool populated = false;
};

struct SkillsSection {
    std::vector<std::string> builtin_names;
    std::size_t cli_spec_count = 0;
    std::size_t cli_spec_skipped = 0;
    std::size_t plugin_spec_count = 0;
    std::size_t plugin_spec_skipped = 0;
    std::string error;
    bool populated = false;
};

struct AgentsSectionRow {
    std::string name;
    bool healthy = false;
    std::vector<std::string> capabilities;
};

struct AgentsSection {
    std::vector<AgentsSectionRow> rows;
    std::string error;
    bool populated = false;
};

struct PluginsSection {
    std::size_t loaded = 0;
    std::size_t skipped = 0;
    std::size_t max_persistent_sessions = 0;
    std::size_t active_sessions = 0;
    std::string error;
    bool populated = false;
};

struct SchedulerSection {
    std::size_t total = 0;
    std::size_t due_now = 0;
    std::size_t disabled = 0;
    std::string error;
    bool populated = false;
};

struct StorageSection {
    std::size_t manifest_files = 0;
    std::string storage_policy;
    std::string error;
    bool populated = false;
};

struct TrustSection {
    std::size_t identities = 0;
    std::size_t paired_devices = 0;
    std::size_t pending_invites = 0;
    std::size_t roles = 0;
    std::size_t pending_approvals = 0;
    std::string error;
    bool populated = false;
};

// ---------------------------------------------------------------------------
// Section collectors.  Each one reads-only.
// ---------------------------------------------------------------------------

PlatformSection CollectPlatform(const std::filesystem::path& workspace) {
    PlatformSection section;
    try {
        section.os_name = kPlatformName;
        section.workspace = workspace.string();
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

BuildSection CollectBuild() {
    BuildSection section;
    try {
        section.version = kAgentOsVersion;
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

AuthSection CollectAuth(
    AuthManager& auth_manager,
    const SessionStore& session_store,
    const AuthProfileStore& auth_profile_store,
    const SecureTokenStore& token_store) {
    AuthSection section;
    try {
        const auto sessions = session_store.list();
        const auto descriptors = auth_manager.providers();

        for (const auto& descriptor : descriptors) {
            AuthProviderRow row;
            row.provider = descriptor.provider_name;

            const auto default_profile = auth_profile_store.default_profile(descriptor.provider);
            row.default_profile = default_profile.value_or("");

            int count = 0;
            for (const auto& session : sessions) {
                if (session.provider == descriptor.provider) {
                    ++count;
                }
            }
            row.session_count = count;

            section.providers.push_back(std::move(row));
        }

        const auto status = token_store.status();
        section.credential_backend = status.backend_name;
        section.credential_keychain_backed = status.system_keychain_backed;
        section.credential_message = status.message;
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

SkillsSection CollectSkills(
    const SkillRegistry& skill_registry,
    const std::filesystem::path& workspace) {
    SkillsSection section;
    try {
        for (const auto& manifest : skill_registry.list()) {
            section.builtin_names.push_back(manifest.name);
        }
        std::sort(section.builtin_names.begin(), section.builtin_names.end());

        const auto cli_loaded = LoadCliSpecsWithDiagnostics(workspace / "runtime" / "cli_specs");
        section.cli_spec_count = cli_loaded.specs.size();
        section.cli_spec_skipped = cli_loaded.diagnostics.size();

        const auto plugin_loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");
        section.plugin_spec_count = plugin_loaded.specs.size();
        section.plugin_spec_skipped = plugin_loaded.diagnostics.size();

        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

AgentsSection CollectAgents(const AgentRegistry& agent_registry) {
    AgentsSection section;
    try {
        for (const auto& profile : agent_registry.list_profiles()) {
            AgentsSectionRow row;
            row.name = profile.agent_name;
            const auto agent = agent_registry.find(profile.agent_name);
            row.healthy = agent && agent->healthy();
            for (const auto& capability : profile.capabilities) {
                row.capabilities.push_back(capability.name);
            }
            section.rows.push_back(std::move(row));
        }
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

PluginsSection CollectPlugins(
    const PluginHost& plugin_host,
    const std::filesystem::path& workspace) {
    PluginsSection section;
    try {
        const auto loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");
        section.loaded = loaded.specs.size();
        section.skipped = loaded.diagnostics.size();

        // The PluginHost stores its options privately and there is no public
        // accessor for max_persistent_sessions, but LoadPluginHostOptions is
        // pure read-only over runtime/plugin_host.tsv and matches whatever the
        // running PluginHost was constructed with.
        const auto options = LoadPluginHostOptions(workspace);
        section.max_persistent_sessions = options.max_persistent_sessions;

        section.active_sessions = plugin_host.active_session_count();
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

SchedulerSection CollectScheduler(const Scheduler& scheduler) {
    SchedulerSection section;
    try {
        const auto tasks = scheduler.list();
        section.total = tasks.size();
        const auto due = scheduler.due(Scheduler::NowEpochMs());
        section.due_now = due.size();
        section.disabled = static_cast<std::size_t>(
            std::count_if(tasks.begin(), tasks.end(), [](const ScheduledTask& task) {
                return !task.enabled;
            }));
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

StorageSection CollectStorage(const StorageVersionStore& storage_version_store) {
    StorageSection section;
    try {
        section.manifest_files = storage_version_store.list().size();
        const auto policy = CurrentStoragePolicy();
        section.storage_policy = policy.backend;
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

TrustSection CollectTrust(
    const IdentityManager& identity_manager,
    const AllowlistStore& allowlist_store,
    const PairingInviteStore& pairing_invite_store,
    const RoleCatalog& role_catalog,
    const ApprovalStore& approval_store) {
    TrustSection section;
    try {
        section.identities = identity_manager.list().size();
        section.paired_devices = allowlist_store.list().size();
        section.pending_invites = pairing_invite_store.list_active().size();
        section.roles = role_catalog.list_roles().size();
        const auto approvals = approval_store.list();
        section.pending_approvals = static_cast<std::size_t>(
            std::count_if(approvals.begin(), approvals.end(), [](const ApprovalRecord& record) {
                return record.status == "pending";
            }));
        section.populated = true;
    } catch (const std::exception& exception) {
        section.error = exception.what();
    }
    return section;
}

// ---------------------------------------------------------------------------
// Renderers.
// ---------------------------------------------------------------------------

void RenderText(
    const PlatformSection& platform,
    const BuildSection& build,
    const AuthSection& auth,
    const SkillsSection& skills,
    const AgentsSection& agents,
    const PluginsSection& plugins,
    const SchedulerSection& scheduler,
    const StorageSection& storage,
    const TrustSection& trust) {
    std::cout << "[platform]\n";
    if (!platform.error.empty()) {
        std::cout << "platform: ERROR " << platform.error << '\n';
    } else {
        std::cout << "  os=" << platform.os_name << '\n';
        std::cout << "  workspace=" << platform.workspace << '\n';
    }

    std::cout << "[build]\n";
    if (!build.error.empty()) {
        std::cout << "build: ERROR " << build.error << '\n';
    } else {
        std::cout << "  version=" << build.version << '\n';
    }

    std::cout << "[auth]\n";
    if (!auth.error.empty()) {
        std::cout << "auth: ERROR " << auth.error << '\n';
    } else {
        std::cout
            << "  credential_backend=" << auth.credential_backend
            << " keychain_backed=" << (auth.credential_keychain_backed ? "true" : "false")
            << '\n';
        for (const auto& row : auth.providers) {
            std::cout
                << "  provider=" << row.provider
                << " default_profile=" << (row.default_profile.empty() ? "(none)" : row.default_profile)
                << " sessions=" << row.session_count
                << '\n';
        }
    }

    std::cout << "[skills]\n";
    if (!skills.error.empty()) {
        std::cout << "skills: ERROR " << skills.error << '\n';
    } else {
        std::cout << "  builtin=" << JoinStrings(skills.builtin_names) << '\n';
        std::cout
            << "  cli_specs=" << skills.cli_spec_count
            << " cli_specs_skipped=" << skills.cli_spec_skipped
            << '\n';
        std::cout
            << "  plugin_specs=" << skills.plugin_spec_count
            << " plugin_specs_skipped=" << skills.plugin_spec_skipped
            << '\n';
    }

    std::cout << "[agents]\n";
    if (!agents.error.empty()) {
        std::cout << "agents: ERROR " << agents.error << '\n';
    } else {
        for (const auto& row : agents.rows) {
            std::cout
                << "  " << row.name
                << " healthy=" << (row.healthy ? "true" : "false")
                << " capabilities=" << JoinStrings(row.capabilities)
                << '\n';
        }
    }

    std::cout << "[plugins]\n";
    if (!plugins.error.empty()) {
        std::cout << "plugins: ERROR " << plugins.error << '\n';
    } else {
        std::cout
            << "  loaded=" << plugins.loaded
            << " skipped=" << plugins.skipped
            << " max_persistent_sessions=" << plugins.max_persistent_sessions
            << " active_sessions=" << plugins.active_sessions
            << '\n';
    }

    std::cout << "[scheduler]\n";
    if (!scheduler.error.empty()) {
        std::cout << "scheduler: ERROR " << scheduler.error << '\n';
    } else {
        std::cout
            << "  total=" << scheduler.total
            << " due_now=" << scheduler.due_now
            << " disabled=" << scheduler.disabled
            << '\n';
    }

    std::cout << "[storage]\n";
    if (!storage.error.empty()) {
        std::cout << "storage: ERROR " << storage.error << '\n';
    } else {
        std::cout
            << "  manifest_files=" << storage.manifest_files
            << " storage_policy=" << storage.storage_policy
            << '\n';
    }

    std::cout << "[trust]\n";
    if (!trust.error.empty()) {
        std::cout << "trust: ERROR " << trust.error << '\n';
    } else {
        std::cout
            << "  identities=" << trust.identities
            << " paired_devices=" << trust.paired_devices
            << " pending_invites=" << trust.pending_invites
            << " roles=" << trust.roles
            << " pending_approvals=" << trust.pending_approvals
            << '\n';
    }
}

nlohmann::ordered_json AuthProviderRowAsJson(const AuthProviderRow& row) {
    nlohmann::ordered_json output;
    output["provider"] = row.provider;
    output["default_profile"] = row.default_profile;
    output["session_count"] = row.session_count;
    return output;
}

nlohmann::ordered_json AgentRowAsJson(const AgentsSectionRow& row) {
    nlohmann::ordered_json output;
    output["name"] = row.name;
    output["healthy"] = row.healthy;
    output["capabilities"] = row.capabilities;
    return output;
}

void RenderJson(
    const PlatformSection& platform,
    const BuildSection& build,
    const AuthSection& auth,
    const SkillsSection& skills,
    const AgentsSection& agents,
    const PluginsSection& plugins,
    const SchedulerSection& scheduler,
    const StorageSection& storage,
    const TrustSection& trust) {
    nlohmann::ordered_json output;

    // platform
    if (!platform.error.empty()) {
        output["platform_error"] = platform.error;
    } else {
        output["platform"] = {
            {"os", platform.os_name},
            {"workspace", platform.workspace},
        };
    }

    // build
    if (!build.error.empty()) {
        output["build_error"] = build.error;
    } else {
        output["build"] = {
            {"version", build.version},
        };
    }

    // auth
    if (!auth.error.empty()) {
        output["auth_error"] = auth.error;
    } else {
        nlohmann::ordered_json providers = nlohmann::ordered_json::array();
        for (const auto& row : auth.providers) {
            providers.push_back(AuthProviderRowAsJson(row));
        }
        output["auth"] = {
            {"credential_backend", auth.credential_backend},
            {"credential_keychain_backed", auth.credential_keychain_backed},
            {"credential_message", auth.credential_message},
            {"providers", std::move(providers)},
        };
    }

    // skills
    if (!skills.error.empty()) {
        output["skills_error"] = skills.error;
    } else {
        output["skills"] = {
            {"builtin", skills.builtin_names},
            {"cli_specs", skills.cli_spec_count},
            {"cli_specs_skipped", skills.cli_spec_skipped},
            {"plugin_specs", skills.plugin_spec_count},
            {"plugin_specs_skipped", skills.plugin_spec_skipped},
        };
    }

    // agents
    if (!agents.error.empty()) {
        output["agents_error"] = agents.error;
    } else {
        nlohmann::ordered_json agent_rows = nlohmann::ordered_json::array();
        for (const auto& row : agents.rows) {
            agent_rows.push_back(AgentRowAsJson(row));
        }
        output["agents"] = std::move(agent_rows);
    }

    // plugins
    if (!plugins.error.empty()) {
        output["plugins_error"] = plugins.error;
    } else {
        output["plugins"] = {
            {"loaded", plugins.loaded},
            {"skipped", plugins.skipped},
            {"max_persistent_sessions", plugins.max_persistent_sessions},
            {"active_sessions", plugins.active_sessions},
        };
    }

    // scheduler
    if (!scheduler.error.empty()) {
        output["scheduler_error"] = scheduler.error;
    } else {
        output["scheduler"] = {
            {"total", scheduler.total},
            {"due_now", scheduler.due_now},
            {"disabled", scheduler.disabled},
        };
    }

    // storage
    if (!storage.error.empty()) {
        output["storage_error"] = storage.error;
    } else {
        output["storage"] = {
            {"manifest_files", storage.manifest_files},
            {"storage_policy", storage.storage_policy},
        };
    }

    // trust
    if (!trust.error.empty()) {
        output["trust_error"] = trust.error;
    } else {
        output["trust"] = {
            {"identities", trust.identities},
            {"paired_devices", trust.paired_devices},
            {"pending_invites", trust.pending_invites},
            {"roles", trust.roles},
            {"pending_approvals", trust.pending_approvals},
        };
    }

    std::cout << output.dump() << '\n';
}

}  // namespace

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
    const int argc,
    char* argv[]) {
    const auto options = ParseOptions(argc, argv, 2);
    const auto format = options.contains("format") ? options.at("format") : "text";
    if (format != "text" && format != "json") {
        std::cerr << "format must be one of: text, json\n";
        return 1;
    }

    const auto platform = CollectPlatform(workspace);
    const auto build = CollectBuild();
    const auto auth = CollectAuth(auth_manager, session_store, auth_profile_store, token_store);
    const auto skills = CollectSkills(skill_registry, workspace);
    const auto agents = CollectAgents(agent_registry);
    const auto plugins = CollectPlugins(plugin_host, workspace);
    const auto scheduler_section = CollectScheduler(scheduler);
    const auto storage = CollectStorage(storage_version_store);
    const auto trust = CollectTrust(identity_manager, allowlist_store, pairing_invite_store, role_catalog, approval_store);

    if (format == "json") {
        RenderJson(platform, build, auth, skills, agents, plugins, scheduler_section, storage, trust);
    } else {
        RenderText(platform, build, auth, skills, agents, plugins, scheduler_section, storage, trust);
    }

    const bool any_error =
        !platform.error.empty() || !build.error.empty() || !auth.error.empty() ||
        !skills.error.empty() || !agents.error.empty() || !plugins.error.empty() ||
        !scheduler_section.error.empty() || !storage.error.empty() || !trust.error.empty();
    return any_error ? 1 : 0;
}

}  // namespace agentos
