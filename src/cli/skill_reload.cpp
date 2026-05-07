#include "cli/skill_reload.hpp"

#include "core/audit/audit_logger.hpp"
#include "core/registry/skill_registry.hpp"
#include "hosts/cli/cli_host.hpp"
#include "hosts/cli/cli_skill_invoker.hpp"
#include "hosts/cli/cli_spec_loader.hpp"
#include "hosts/plugin/plugin_host.hpp"
#include "utils/command_utils.hpp"

#include <memory>
#include <utility>

namespace agentos {

namespace {

struct RuntimeSkillReloadState {
    std::unordered_set<std::string> cli_specs;
    std::unordered_set<std::string> plugin_specs;
};

RuntimeSkillReloadState& InteractiveReloadState() {
    static RuntimeSkillReloadState state;
    return state;
}

std::string CliSpecConflictReason(const std::string& name) {
    return "external CLI spec name conflicts with protected runtime skill: " + name;
}

std::string PluginSpecConflictReason(const std::string& name) {
    return "plugin spec name conflicts with protected runtime skill or CLI spec: " + name;
}

}  // namespace

std::unordered_set<std::string> ProtectedSkillNames() {
    return {
        "file_read",
        "file_write",
        "file_patch",
        "http_fetch",
        "news_search",
        "host_info",
        "learn_skill",
        "development_request",
        "research_request",
        "workflow_run",
        "rg_search",
        "git_status",
        "git_diff",
        "curl_fetch",
        "jq_transform",
    };
}

void ReloadExternalSkills(SkillRegistry& skill_registry,
                          CliHost& cli_host,
                          PluginHost& plugin_host,
                          AuditLogger& audit_logger,
                          const std::filesystem::path& workspace) {
    auto& state = InteractiveReloadState();
    const auto protected_names = ProtectedSkillNames();
    const auto cli_loaded = LoadCliSpecsWithDiagnostics(workspace / "runtime" / "cli_specs");
    const auto plugin_loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");

    std::unordered_set<std::string> desired_cli;
    for (const auto& spec : cli_loaded.specs) {
        desired_cli.insert(spec.name);
    }
    std::unordered_set<std::string> desired_plugin;
    for (const auto& spec : plugin_loaded.specs) {
        desired_plugin.insert(spec.name);
    }

    for (const auto& name : state.cli_specs) {
        if (!desired_cli.contains(name) && !desired_plugin.contains(name)) {
            skill_registry.unregister_skill(name);
        }
    }
    for (const auto& name : state.plugin_specs) {
        if (!desired_plugin.contains(name) && !desired_cli.contains(name)) {
            skill_registry.unregister_skill(name);
        }
    }

    std::unordered_set<std::string> active_cli;
    for (const auto& spec : cli_loaded.specs) {
        if (protected_names.contains(spec.name)) {
            audit_logger.record_config_diagnostic(
                "cli_spec", spec.source_file, spec.source_line_number,
                CliSpecConflictReason(spec.name));
            continue;
        }
        if (!CommandExists(spec.binary)) {
            audit_logger.record_config_diagnostic(
                "cli_spec", spec.source_file, spec.source_line_number,
                "CLI spec binary is not available on this host: " + spec.binary);
            continue;
        }
        skill_registry.register_skill(std::make_shared<CliSkillInvoker>(spec, cli_host));
        active_cli.insert(spec.name);
    }

    std::unordered_set<std::string> active_plugin;
    for (const auto& spec : plugin_loaded.specs) {
        if (protected_names.contains(spec.name) || active_cli.contains(spec.name)) {
            audit_logger.record_config_diagnostic(
                "plugin_spec", spec.source_file, spec.source_line_number,
                PluginSpecConflictReason(spec.name));
            continue;
        }
        skill_registry.register_skill(std::make_shared<PluginSkillInvoker>(spec, plugin_host));
        active_plugin.insert(spec.name);
    }

    for (const auto& diagnostic : cli_loaded.diagnostics) {
        audit_logger.record_config_diagnostic(
            "cli_spec", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
    for (const auto& diagnostic : plugin_loaded.diagnostics) {
        audit_logger.record_config_diagnostic(
            "plugin_spec", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }

    state.cli_specs = std::move(active_cli);
    state.plugin_specs = std::move(active_plugin);
}

}  // namespace agentos
