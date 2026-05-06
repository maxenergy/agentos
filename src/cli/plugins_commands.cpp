#include "cli/plugins_commands.hpp"

#include "hosts/cli/cli_host.hpp"
#include "hosts/cli/cli_spec_loader.hpp"
#include "hosts/plugin/plugin_host.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace agentos {

namespace {

void PrintSpecDiagnostic(
    const std::string& label,
    const std::filesystem::path& file,
    const int line_number,
    const std::string& reason) {
    std::cout
        << label
        << " file=" << file.string()
        << " line=" << line_number
        << " reason=" << reason
        << '\n';
}

std::set<std::string> PluginValidateConflictNames(
    const std::filesystem::path& workspace,
    const std::set<std::string>& builtin_skill_names) {
    auto names = builtin_skill_names;
    const auto cli_specs = LoadCliSpecsWithDiagnostics(workspace / "runtime" / "cli_specs");
    for (const auto& spec : cli_specs.specs) {
        names.insert(spec.name);
    }
    return names;
}

std::string PluginIsolationProfile(const PluginSpec& spec);
bool HasPluginResourceLimits(const PluginSpec& spec);

bool PrintPlugins(
    const std::filesystem::path& workspace,
    const std::set<std::string>& conflict_names,
    const bool check_command_health) {
    bool all_healthy = true;
    const auto loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");
    CliHost cli_host;
    for (const auto& spec : loaded.specs) {
        const bool conflicts_with_registered_skill = conflict_names.contains(spec.name);
        std::cout
            << spec.name
            << " protocol=" << spec.protocol
            << " manifest_version=" << spec.manifest_version
            << " sandbox_mode=" << spec.sandbox_mode
            << " isolation_profile=" << PluginIsolationProfile(spec)
            << " resource_limits_configured=" << (HasPluginResourceLimits(spec) ? "true" : "false")
            << " lifecycle_mode=" << spec.lifecycle_mode
            << " startup_timeout_ms=" << spec.startup_timeout_ms
            << " idle_timeout_ms=" << spec.idle_timeout_ms
            << " pool_size=" << spec.pool_size
            << " source=" << spec.source_file.string()
            << " line=" << spec.source_line_number;
        if (check_command_health) {
            const auto health = CheckPluginHealth(spec, cli_host, workspace);
            all_healthy = all_healthy && health.healthy && !conflicts_with_registered_skill;
            std::cout
                << " healthy=" << (health.healthy && !conflicts_with_registered_skill ? "true" : "false")
                << " command_available=" << (health.command_available ? "true" : "false")
                << " reason=" << (conflicts_with_registered_skill
                    ? PluginSpecConflictReason(spec.name)
                    : health.reason);
        } else {
            std::cout << " valid=" << (conflicts_with_registered_skill ? "false" : "true");
            if (conflicts_with_registered_skill) {
                std::cout << " reason=" << PluginSpecConflictReason(spec.name);
            }
        }
        std::cout << '\n';
        if (conflicts_with_registered_skill) {
            PrintSpecDiagnostic(
                "conflicting_plugin",
                spec.source_file,
                spec.source_line_number,
                PluginSpecConflictReason(spec.name));
        }
    }
    for (const auto& diagnostic : loaded.diagnostics) {
        PrintSpecDiagnostic("skipped_plugin", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
    const bool no_conflicts = std::none_of(loaded.specs.begin(), loaded.specs.end(), [&](const PluginSpec& spec) {
        return conflict_names.contains(spec.name);
    });
    return all_healthy && loaded.diagnostics.empty() && no_conflicts;
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

std::string InspectPluginName(const int argc, char* argv[]) {
    if (argc < 4) {
        return "";
    }
    std::string value = argv[3];
    constexpr std::string_view kPrefix = "name=";
    if (value.starts_with(kPrefix)) {
        value.erase(0, kPrefix.size());
    }
    return value;
}

bool InspectShouldCheckHealth(const int argc, char* argv[]) {
    for (int index = 4; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "health=true" || option == "check_health=true") {
            return true;
        }
    }
    return false;
}

bool HasPluginResourceLimits(const PluginSpec& spec) {
    return spec.memory_limit_bytes != 0 ||
        spec.max_processes != 0 ||
        spec.cpu_time_limit_seconds != 0 ||
        spec.file_descriptor_limit != 0;
}

std::string PluginIsolationProfile(const PluginSpec& spec) {
    std::string profile;
    if (spec.sandbox_mode == "workspace") {
        profile = "workspace-paths";
    }
    if (HasPluginResourceLimits(spec)) {
        if (!profile.empty()) {
            profile += "+";
        }
        profile += "process-resource-limits";
    }
    return profile.empty() ? "none" : profile;
}

std::size_t EffectivePersistentPoolSize(const PluginSpec& spec, const std::size_t max_persistent_sessions) {
    if (spec.lifecycle_mode != "persistent") {
        return 0;
    }
    const auto requested = static_cast<std::size_t>((std::max)(1, spec.pool_size));
    return max_persistent_sessions == 0
        ? requested
        : (std::min)(requested, max_persistent_sessions);
}

std::string PluginProcessPoolPolicy() {
    return "per_plugin_workspace_binary_lru";
}

void PrintPluginHostConfigDiagnostics(const std::vector<PluginHostOptionsDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cout
            << "plugin_host_config_diagnostic"
            << " line=" << diagnostic.line_number
            << " reason=\"" << diagnostic.reason << "\""
            << '\n';
    }
}

void PrintPluginInspect(
    const PluginSpec& spec,
    const bool conflicts_with_registered_skill,
    const std::size_t max_persistent_sessions,
    const std::optional<PluginHealthStatus>& health = std::nullopt) {
    const auto effective_pool_size = EffectivePersistentPoolSize(spec, max_persistent_sessions);
    std::cout
        << "name=" << spec.name << '\n'
        << "description=" << spec.description << '\n'
        << "manifest_version=" << spec.manifest_version << '\n'
        << "protocol=" << spec.protocol << '\n'
        << "binary=" << spec.binary << '\n'
        << "args_template=" << JoinStrings(spec.args_template) << '\n'
        << "required_args=" << JoinStrings(spec.required_args) << '\n'
        << "risk_level=" << spec.risk_level << '\n'
        << "permissions=" << JoinStrings(spec.permissions) << '\n'
        << "timeout_ms=" << spec.timeout_ms << '\n'
        << "output_limit_bytes=" << spec.output_limit_bytes << '\n'
        << "env_allowlist=" << JoinStrings(spec.env_allowlist) << '\n'
        << "idempotent=" << (spec.idempotent ? "true" : "false") << '\n'
        << "memory_limit_bytes=" << spec.memory_limit_bytes << '\n'
        << "max_processes=" << spec.max_processes << '\n'
        << "cpu_time_limit_seconds=" << spec.cpu_time_limit_seconds << '\n'
        << "file_descriptor_limit=" << spec.file_descriptor_limit << '\n'
        << "health_args_template=" << JoinStrings(spec.health_args_template) << '\n'
        << "health_timeout_ms=" << spec.health_timeout_ms << '\n'
        << "sandbox_mode=" << spec.sandbox_mode << '\n'
        << "isolation_profile=" << PluginIsolationProfile(spec) << '\n'
        << "resource_limits_configured=" << (HasPluginResourceLimits(spec) ? "true" : "false") << '\n'
        << "lifecycle_mode=" << spec.lifecycle_mode << '\n'
        << "startup_timeout_ms=" << spec.startup_timeout_ms << '\n'
        << "idle_timeout_ms=" << spec.idle_timeout_ms << '\n'
        << "pool_size=" << spec.pool_size << '\n'
        << "max_persistent_sessions=" << max_persistent_sessions << '\n'
        << "effective_pool_size=" << effective_pool_size << '\n'
        << "pool_policy=" << PluginProcessPoolPolicy() << '\n'
        << "source=" << spec.source_file.string() << '\n'
        << "line=" << spec.source_line_number << '\n'
        << "valid=" << (conflicts_with_registered_skill ? "false" : "true") << '\n';
    if (conflicts_with_registered_skill) {
        std::cout << "reason=" << PluginSpecConflictReason(spec.name) << '\n';
    }
    if (health.has_value()) {
        std::cout
            << "healthy=" << (health->healthy && !conflicts_with_registered_skill ? "true" : "false") << '\n'
            << "command_available=" << (health->command_available ? "true" : "false") << '\n'
            << "health_reason=" << (conflicts_with_registered_skill
                ? PluginSpecConflictReason(spec.name)
                : health->reason) << '\n';
    }
}

int InspectPlugin(
    const std::filesystem::path& workspace,
    const std::set<std::string>& conflict_names,
    const int argc,
    char* argv[]) {
    const auto name = InspectPluginName(argc, argv);
    const bool check_health = InspectShouldCheckHealth(argc, argv);
    if (name.empty()) {
        std::cout << "missing required plugin name; use: agentos plugins inspect name=<plugin_name>\n";
        return 2;
    }

    const auto loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");
    const auto host_options = LoadPluginHostOptionsWithDiagnostics(workspace);
    for (const auto& spec : loaded.specs) {
        if (spec.name != name) {
            continue;
        }
        const bool conflicts_with_registered_skill = conflict_names.contains(spec.name);
        std::optional<PluginHealthStatus> health;
        if (check_health) {
            CliHost cli_host;
            health = CheckPluginHealth(spec, cli_host, workspace);
        }
        PrintPluginInspect(spec, conflicts_with_registered_skill, host_options.options.max_persistent_sessions, health);
        PrintPluginHostConfigDiagnostics(host_options.diagnostics);
        if (conflicts_with_registered_skill) {
            PrintSpecDiagnostic(
                "conflicting_plugin",
                spec.source_file,
                spec.source_line_number,
                PluginSpecConflictReason(spec.name));
        }
        if (conflicts_with_registered_skill) {
            return 1;
        }
        if (health.has_value() && !health->healthy) {
            return 1;
        }
        return host_options.diagnostics.empty() ? 0 : 1;
    }

    for (const auto& diagnostic : loaded.diagnostics) {
        PrintSpecDiagnostic("skipped_plugin", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
    std::cout << "plugin_not_found name=" << name << '\n';
    return 1;
}

int PrintPluginLifecycle(
    const std::filesystem::path& workspace,
    const std::set<std::string>& conflict_names) {
    const auto loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");
    std::size_t persistent_count = 0;
    std::size_t oneshot_count = 0;
    std::size_t conflict_count = 0;
    const auto host_options = LoadPluginHostOptionsWithDiagnostics(workspace);
    for (const auto& spec : loaded.specs) {
        if (spec.lifecycle_mode == "persistent") {
            ++persistent_count;
        } else {
            ++oneshot_count;
        }
        const bool conflicts_with_registered_skill = conflict_names.contains(spec.name);
        if (conflicts_with_registered_skill) {
            ++conflict_count;
        }
        std::cout
            << "plugin_lifecycle name=" << spec.name
            << " lifecycle_mode=" << spec.lifecycle_mode
            << " protocol=" << spec.protocol
            << " sandbox_mode=" << spec.sandbox_mode
            << " isolation_profile=" << PluginIsolationProfile(spec)
            << " resource_limits_configured=" << (HasPluginResourceLimits(spec) ? "true" : "false")
            << " startup_timeout_ms=" << spec.startup_timeout_ms
            << " idle_timeout_ms=" << spec.idle_timeout_ms
            << " pool_size=" << spec.pool_size
            << " effective_pool_size=" << EffectivePersistentPoolSize(spec, host_options.options.max_persistent_sessions)
            << " pool_policy=" << PluginProcessPoolPolicy()
            << " source=" << spec.source_file.string()
            << " line=" << spec.source_line_number
            << " valid=" << (conflicts_with_registered_skill ? "false" : "true");
        if (conflicts_with_registered_skill) {
            std::cout << " reason=" << PluginSpecConflictReason(spec.name);
        }
        std::cout << '\n';
    }
    for (const auto& diagnostic : loaded.diagnostics) {
        PrintSpecDiagnostic("skipped_plugin", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
    PrintPluginHostConfigDiagnostics(host_options.diagnostics);

    std::cout
        << "plugin_lifecycle_summary"
        << " total=" << loaded.specs.size()
        << " oneshot=" << oneshot_count
        << " persistent=" << persistent_count
        << " max_persistent_sessions=" << host_options.options.max_persistent_sessions
        << " pool_policy=" << PluginProcessPoolPolicy()
        << " scope=process"
        << " persistence=none"
        << " diagnostics=" << loaded.diagnostics.size()
        << " config_diagnostics=" << host_options.diagnostics.size()
        << " conflicts=" << conflict_count
        << '\n';
    return loaded.diagnostics.empty() && host_options.diagnostics.empty() && conflict_count == 0 ? 0 : 1;
}

std::string SessionAdminPluginName(const int argc, char* argv[]) {
    for (int index = 3; index < argc; ++index) {
        std::string value = argv[index];
        constexpr std::string_view kPrefix = "name=";
        if (value.starts_with(kPrefix)) {
            value.erase(0, kPrefix.size());
            return value;
        }
    }
    return {};
}

bool SessionAdminDryRun(const int argc, char* argv[]) {
    for (int index = 3; index < argc; ++index) {
        const std::string value = argv[index];
        if (value == "dry_run=true" || value == "dry-run=true") {
            return true;
        }
    }
    return false;
}

std::string SessionAdminScope(const int argc, char* argv[]) {
    for (int index = 3; index < argc; ++index) {
        std::string value = argv[index];
        constexpr std::string_view kPrefix = "scope=";
        if (value.starts_with(kPrefix)) {
            value.erase(0, kPrefix.size());
            return value;
        }
    }
    return "process";
}

bool RejectUnsupportedSessionScope(const std::string& scope) {
    if (scope == "process") {
        return false;
    }
    std::cout
        << "plugin_sessions_unavailable"
        << " scope=" << scope
        << " supported_scope=process"
        << " reason=\"cross-process plugin session admin not implemented\""
        << '\n';
    return true;
}

int PrintPluginSessions(
    const PluginHost* plugin_host,
    const int argc,
    char* argv[]) {
    if (plugin_host == nullptr) {
        std::cout << "plugin_sessions_unavailable reason=\"plugin host runtime not available\"\n";
        return 2;
    }
    const auto scope = SessionAdminScope(argc, argv);
    if (RejectUnsupportedSessionScope(scope)) {
        return 2;
    }
    const auto filter_name = SessionAdminPluginName(argc, argv);
    const auto sessions = plugin_host->list_sessions();
    std::size_t printed = 0;
    std::size_t expired = 0;
    std::size_t dead = 0;
    for (const auto& session : sessions) {
        if (!filter_name.empty() && session.plugin_name != filter_name) {
            continue;
        }
        if (session.idle_expired) {
            ++expired;
        }
        if (!session.alive) {
            ++dead;
        }
        std::cout
            << "plugin_session"
            << " name=" << session.plugin_name
            << " pid=" << session.pid
            << " workspace=" << session.workspace_path
            << " binary=" << session.binary
            << " started_at_unix_ms=" << session.started_at_unix_ms
            << " last_used_at_unix_ms=" << session.last_used_at_unix_ms
            << " idle_for_ms=" << session.idle_for_ms
            << " idle_timeout_ms=" << session.idle_timeout_ms
            << " idle_expired=" << (session.idle_expired ? "true" : "false")
            << " request_count=" << session.request_count
            << " alive=" << (session.alive ? "true" : "false")
            << '\n';
        ++printed;
    }
    std::cout
        << "plugin_sessions_summary"
        << " total=" << printed
        << " active=" << plugin_host->active_session_count();
    if (!filter_name.empty()) {
        std::cout
            << " name=" << filter_name
            << " matched=" << (printed > 0 ? "true" : "false");
    }
    std::cout
        << " idle_expired=" << expired
        << " dead=" << dead
        << " scope=process"
        << " persistence=none"
        << '\n';
    return 0;
}

int RestartPluginSession(
    const PluginHost* plugin_host,
    const int argc,
    char* argv[]) {
    if (plugin_host == nullptr) {
        std::cout << "plugin_sessions_unavailable reason=\"plugin host runtime not available\"\n";
        return 2;
    }
    const auto scope = SessionAdminScope(argc, argv);
    if (RejectUnsupportedSessionScope(scope)) {
        return 2;
    }
    const auto name = SessionAdminPluginName(argc, argv);
    if (name.empty()) {
        std::cout
            << "missing required plugin name; use: agentos plugins session-restart name=<plugin_name>\n";
        return 2;
    }
    const auto restarted = plugin_host->restart_sessions_for_plugin(name);
    std::cout
        << "plugin_session_restart"
        << " name=" << name
        << " restarted=" << restarted
        << " matched=" << (restarted > 0 ? "true" : "false")
        << " scope=process"
        << " persistence=none"
        << '\n';
    return 0;
}

int ClosePluginSession(
    const PluginHost* plugin_host,
    const int argc,
    char* argv[]) {
    if (plugin_host == nullptr) {
        std::cout << "plugin_sessions_unavailable reason=\"plugin host runtime not available\"\n";
        return 2;
    }
    const auto scope = SessionAdminScope(argc, argv);
    if (RejectUnsupportedSessionScope(scope)) {
        return 2;
    }
    const auto name = SessionAdminPluginName(argc, argv);
    if (name.empty()) {
        std::cout
            << "missing required plugin name; use: agentos plugins session-close name=<plugin_name>\n";
        return 2;
    }
    const auto closed = plugin_host->close_sessions_for_plugin(name);
    std::cout
        << "plugin_session_close"
        << " name=" << name
        << " closed=" << closed
        << " matched=" << (closed > 0 ? "true" : "false")
        << " scope=process"
        << " persistence=none"
        << '\n';
    return 0;
}

int PrunePluginSessions(
    const PluginHost* plugin_host,
    const int argc,
    char* argv[]) {
    if (plugin_host == nullptr) {
        std::cout << "plugin_sessions_unavailable reason=\"plugin host runtime not available\"\n";
        return 2;
    }
    const auto scope = SessionAdminScope(argc, argv);
    if (RejectUnsupportedSessionScope(scope)) {
        return 2;
    }
    const auto name = SessionAdminPluginName(argc, argv);
    const auto dry_run = SessionAdminDryRun(argc, argv);
    const auto would_prune = dry_run ? plugin_host->count_inactive_sessions(name) : std::size_t{0};
    const auto pruned = dry_run ? std::size_t{0} : plugin_host->prune_inactive_sessions(name);
    const auto matched = dry_run ? would_prune > 0 : pruned > 0;
    std::cout
        << "plugin_session_prune";
    if (!name.empty()) {
        std::cout << " name=" << name;
    }
    std::cout
        << " pruned=" << pruned
        << " matched=" << (matched ? "true" : "false")
        << " dry_run=" << (dry_run ? "true" : "false")
        << " would_prune=" << would_prune
        << " reason=idle_expired_or_dead"
        << " scope=process"
        << " persistence=none"
        << '\n';
    return 0;
}

}  // namespace

std::string PluginSpecConflictReason(const std::string& name) {
    return "plugin spec name conflicts with already registered skill: " + name;
}

int RunPluginsCommand(
    const std::filesystem::path& workspace,
    const std::set<std::string>& builtin_skill_names,
    const int argc,
    char* argv[],
    const PluginHost* plugin_host) {
    const auto conflict_names = PluginValidateConflictNames(workspace, builtin_skill_names);
    if (argc >= 3 && std::string(argv[2]) == "inspect") {
        return InspectPlugin(workspace, conflict_names, argc, argv);
    }
    if (argc >= 3 && std::string(argv[2]) == "lifecycle") {
        return PrintPluginLifecycle(workspace, conflict_names);
    }
    if (argc >= 3 && std::string(argv[2]) == "sessions") {
        return PrintPluginSessions(plugin_host, argc, argv);
    }
    if (argc >= 3 && std::string(argv[2]) == "session-restart") {
        return RestartPluginSession(plugin_host, argc, argv);
    }
    if (argc >= 3 && std::string(argv[2]) == "session-close") {
        return ClosePluginSession(plugin_host, argc, argv);
    }
    if (argc >= 3 && std::string(argv[2]) == "session-prune") {
        return PrunePluginSessions(plugin_host, argc, argv);
    }
    if (argc >= 3 && std::string(argv[2]) == "validate") {
        const bool valid = PrintPlugins(workspace, conflict_names, false);
        return valid ? 0 : 1;
    }
    const bool all_healthy = PrintPlugins(workspace, conflict_names, true);
    if (argc >= 3 && std::string(argv[2]) == "health") {
        return all_healthy ? 0 : 1;
    }
    return 0;
}

}  // namespace agentos
