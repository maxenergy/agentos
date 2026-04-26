#include "hosts/plugin/plugin_spec_utils.hpp"

#include "core/policy/permission_model.hpp"
#include "utils/spec_parsing.hpp"

namespace agentos {

namespace {

bool IsSupportedProtocol(const std::string& protocol) {
    return protocol == "stdio-json-v0" || protocol == "json-rpc-v0";
}

bool IsSupportedManifestVersion(const std::string& version) {
    return version == "plugin.v1";
}

bool IsSupportedRiskLevel(const std::string& risk_level) {
    return PermissionModel::parse_risk_level(risk_level) != RiskLevel::unknown;
}

bool IsSupportedSandboxMode(const std::string& sandbox_mode) {
    return sandbox_mode == "workspace" || sandbox_mode == "none";
}

bool IsSupportedLifecycleMode(const std::string& lifecycle_mode) {
    return lifecycle_mode == "oneshot" || lifecycle_mode == "persistent";
}

}  // namespace

std::string PluginSpecUnsupportedReason(const PluginSpec& spec) {
    if (!IsSupportedManifestVersion(spec.manifest_version)) {
        return "unsupported plugin manifest version: " + spec.manifest_version;
    }
    if (spec.name.empty()) {
        return "plugin name is required";
    }
    if (spec.binary.empty()) {
        return "plugin binary is required";
    }
    if (!IsSupportedProtocol(spec.protocol)) {
        return "unsupported plugin protocol: " + spec.protocol;
    }
    if (!IsSupportedRiskLevel(spec.risk_level)) {
        return "unsupported risk_level: " + spec.risk_level;
    }
    if (const auto unknown_permissions = PermissionModel::unknown_permissions(spec.permissions);
        !unknown_permissions.empty()) {
        return "unknown permissions: " + JoinStrings(unknown_permissions);
    }
    if (!PermissionModel::has_permission(spec.permissions, PermissionNames::ProcessSpawn)) {
        return "plugin permissions must include process.spawn";
    }
    if (!IsSupportedSandboxMode(spec.sandbox_mode)) {
        return "unsupported sandbox_mode: " + spec.sandbox_mode;
    }
    if (!IsSupportedLifecycleMode(spec.lifecycle_mode)) {
        return "unsupported lifecycle_mode: " + spec.lifecycle_mode;
    }
    if (spec.lifecycle_mode == "persistent" && spec.protocol != "json-rpc-v0") {
        return "persistent plugin lifecycle requires json-rpc-v0 protocol";
    }
    return {};
}

bool PluginSpecIsSupported(const PluginSpec& spec) {
    return PluginSpecUnsupportedReason(spec).empty();
}

CliSpec ToCliSpec(const PluginSpec& spec) {
    return {
        .name = spec.name,
        .description = spec.description,
        .binary = spec.binary,
        .args_template = spec.args_template,
        .required_args = spec.required_args,
        .input_schema_json = spec.input_schema_json,
        .output_schema_json = spec.output_schema_json,
        .parse_mode = spec.protocol,
        .risk_level = spec.risk_level,
        .permissions = spec.permissions,
        .timeout_ms = spec.timeout_ms,
        .output_limit_bytes = spec.output_limit_bytes,
        .env_allowlist = spec.env_allowlist,
        .memory_limit_bytes = spec.memory_limit_bytes,
        .max_processes = spec.max_processes,
        .cpu_time_limit_seconds = spec.cpu_time_limit_seconds,
        .file_descriptor_limit = spec.file_descriptor_limit,
    };
}

CliSpec ToPluginHealthCliSpec(const PluginSpec& spec) {
    auto cli_spec = ToCliSpec(spec);
    cli_spec.name = spec.name + "_health";
    cli_spec.description = "Plugin health probe for " + spec.name;
    cli_spec.args_template = spec.health_args_template;
    cli_spec.required_args = {};
    cli_spec.timeout_ms = spec.health_timeout_ms;
    return cli_spec;
}

}  // namespace agentos
