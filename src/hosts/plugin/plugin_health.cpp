#include "hosts/plugin/plugin_host.hpp"

#include "hosts/plugin/plugin_json_rpc.hpp"
#include "hosts/plugin/plugin_persistent_session.hpp"
#include "hosts/plugin/plugin_spec_utils.hpp"
#include "utils/command_utils.hpp"

namespace agentos {

PluginHealthStatus CheckPluginHealth(const PluginSpec& spec) {
    const auto unsupported_reason = PluginSpecUnsupportedReason(spec);
    if (!unsupported_reason.empty()) {
        return {
            .supported = false,
            .command_available = false,
            .healthy = false,
            .reason = unsupported_reason,
        };
    }

    const bool command_available = CommandExists(spec.binary);
    return {
        .supported = true,
        .command_available = command_available,
        .healthy = command_available,
        .reason = command_available ? "ok" : "plugin binary not found: " + spec.binary,
    };
}

PluginHealthStatus CheckPluginHealth(
    const PluginSpec& spec,
    const CliHost& cli_host,
    const std::filesystem::path& workspace_path) {
    auto health = CheckPluginHealth(spec);
    if (health.healthy && spec.lifecycle_mode == "persistent") {
        auto health_spec = spec;
        health_spec.timeout_ms = spec.health_timeout_ms;
        std::string start_error;
        auto session = PersistentPluginSession::start(health_spec, workspace_path, start_error);
        if (!session) {
            return {
                .supported = true,
                .command_available = true,
                .healthy = false,
                .reason = "persistent health startup failed: " + start_error,
            };
        }
        auto probe = session->request({}, true);
        if (!probe.success) {
            return {
                .supported = true,
                .command_available = true,
                .healthy = false,
                .reason = probe.error_message.empty()
                    ? "persistent health round-trip failed"
                    : "persistent health round-trip failed: " + probe.error_message,
            };
        }
        if (const auto json_rpc_error = JsonRpcOutputError(probe.stdout_text); !json_rpc_error.empty()) {
            return {
                .supported = true,
                .command_available = true,
                .healthy = false,
                .reason = "persistent health round-trip failed: " + json_rpc_error,
            };
        }
        return {
            .supported = true,
            .command_available = true,
            .healthy = true,
            .reason = "ok",
        };
    }
    if (!health.healthy || spec.health_args_template.empty()) {
        return health;
    }

    const auto probe = cli_host.run(CliRunRequest{
        .spec = ToPluginHealthCliSpec(spec),
        .workspace_path = workspace_path,
    });
    if (probe.success) {
        return {
            .supported = true,
            .command_available = true,
            .healthy = true,
            .reason = "ok",
        };
    }

    return {
        .supported = true,
        .command_available = true,
        .healthy = false,
        .reason = probe.error_message.empty()
            ? "health probe failed with exit_code=" + std::to_string(probe.exit_code)
            : "health probe failed: " + probe.error_message,
    };
}

}  // namespace agentos
