#include "hosts/plugin/plugin_host.hpp"

#include "core/policy/permission_model.hpp"
#include "hosts/plugin/plugin_json_rpc.hpp"
#include "hosts/plugin/plugin_json_utils.hpp"
#include "hosts/plugin/plugin_persistent_session.hpp"
#include "hosts/plugin/plugin_sandbox.hpp"
#include "hosts/plugin/plugin_schema_validator.hpp"
#include "hosts/plugin/plugin_spec_utils.hpp"
#include "utils/command_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentos {

PluginHostOptionsLoadResult LoadPluginHostOptionsWithDiagnostics(const std::filesystem::path& workspace_path) {
    PluginHostOptionsLoadResult result;
    std::ifstream input(workspace_path / "runtime" / "plugin_host.tsv", std::ios::binary);
    if (!input) {
        return result;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto fields = SplitTsvFields(line);
        if (fields.size() < 2) {
            result.diagnostics.push_back(PluginHostOptionsDiagnostic{
                .line_number = line_number,
                .reason = "expected key and value fields",
            });
            continue;
        }
        if (fields[0] == "max_persistent_sessions") {
            std::size_t parsed = 0;
            if (ParseStrictSize(fields[1], parsed)) {
                result.options.max_persistent_sessions = parsed;
            } else {
                result.diagnostics.push_back(PluginHostOptionsDiagnostic{
                    .line_number = line_number,
                    .reason = "invalid max_persistent_sessions: " + fields[1],
                });
            }
        } else {
            result.diagnostics.push_back(PluginHostOptionsDiagnostic{
                .line_number = line_number,
                .reason = "unknown plugin host option: " + fields[0],
            });
        }
    }
    return result;
}

PluginHostOptions LoadPluginHostOptions(const std::filesystem::path& workspace_path) {
    return LoadPluginHostOptionsWithDiagnostics(workspace_path).options;
}

PluginHost::PluginHost(const CliHost& cli_host, PluginHostOptions options)
    : cli_host_(cli_host),
      options_(options) {}

PluginHost::~PluginHost() = default;

std::size_t PluginHost::active_session_count() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::size_t active = 0;
    for (const auto& [unused_key, session] : sessions_) {
        (void)unused_key;
        if (session && session->alive()) {
            ++active;
        }
    }
    return active;
}

std::size_t PluginHost::close_all_sessions() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto count = sessions_.size();
    sessions_.clear();
    return count;
}

namespace {

void PruneInactivePersistentSessions(
    std::map<std::string, std::unique_ptr<PersistentPluginSession>>& sessions) {
    for (auto iterator = sessions.begin(); iterator != sessions.end();) {
        if (!iterator->second || !iterator->second->alive()) {
            iterator = sessions.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void EvictOldestPersistentSession(
    std::map<std::string, std::unique_ptr<PersistentPluginSession>>& sessions,
    const std::string& requested_session_key,
    const std::size_t max_sessions) {
    if (max_sessions == 0 || sessions.contains(requested_session_key)) {
        return;
    }

    PruneInactivePersistentSessions(sessions);
    while (sessions.size() >= max_sessions) {
        auto oldest = sessions.end();
        for (auto iterator = sessions.begin(); iterator != sessions.end(); ++iterator) {
            if (iterator->first == requested_session_key || !iterator->second) {
                continue;
            }
            if (oldest == sessions.end() ||
                iterator->second->last_used_at < oldest->second->last_used_at) {
                oldest = iterator;
            }
        }
        if (oldest == sessions.end()) {
            break;
        }
        sessions.erase(oldest);
    }
}

}  // namespace

PluginRunResult PluginHost::run(const PluginRunRequest& request) const {
    if (!PluginSpecIsSupported(request.spec)) {
        return {
            .success = false,
            .error_code = "InvalidPluginSpec",
            .error_message = PluginSpecUnsupportedReason(request.spec),
        };
    }
    if (const auto sandbox_error = PluginSandboxError(request); !sandbox_error.empty()) {
        return {
            .success = false,
            .error_code = "PluginSandboxDenied",
            .error_message = sandbox_error,
        };
    }
    PluginRunResult plugin_result;
    if (request.spec.lifecycle_mode == "persistent") {
        const auto session_key =
            request.spec.name + "\n" + request.workspace_path.string() + "\n" + request.spec.binary;
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        EvictOldestPersistentSession(sessions_, session_key, options_.max_persistent_sessions);
        auto& session = sessions_[session_key];
        const bool had_session = static_cast<bool>(session);
        const bool idle_expired = session && session->alive() && session->idle_expired(request.spec.idle_timeout_ms);
        if (idle_expired) {
            session.reset();
        }
        if (!session || !session->alive() || idle_expired) {
            std::string start_error;
            session = PersistentPluginSession::start(request.spec, request.workspace_path, start_error);
            if (!session) {
                return {
                    .success = false,
                    .error_code = "PluginLifecycleStartFailed",
                    .error_message = start_error,
                    .lifecycle_event = had_session ? "restart_failed" : "start_failed",
                };
            }
            if (!session->wait_until_started(request.spec.startup_timeout_ms)) {
                session.reset();
                return {
                    .success = false,
                    .error_code = "PluginLifecycleStartFailed",
                    .error_message = "persistent plugin process exited during startup",
                    .lifecycle_event = had_session ? "restart_failed" : "start_failed",
                };
            }
            plugin_result.lifecycle_event = idle_expired ? "idle_restarted" : (had_session ? "restarted" : "started");
        } else {
            plugin_result.lifecycle_event = "reused";
        }
        auto request_result = session->request(request.arguments);
        request_result.lifecycle_event = plugin_result.lifecycle_event;
        if (!request_result.success && !request_result.timed_out) {
            sessions_.erase(session_key);
            std::string restart_error;
            auto restarted_session = PersistentPluginSession::start(request.spec, request.workspace_path, restart_error);
            if (restarted_session && restarted_session->wait_until_started(request.spec.startup_timeout_ms)) {
                auto retry_result = restarted_session->request(request.arguments);
                retry_result.lifecycle_event = "restarted";
                session = std::move(restarted_session);
                request_result = std::move(retry_result);
            } else {
                request_result.lifecycle_event = "restart_failed";
                request_result.error_code = "PluginLifecycleStartFailed";
                request_result.error_message = restart_error.empty()
                    ? "persistent plugin process exited during restart"
                    : restart_error;
            }
        }
        plugin_result = std::move(request_result);
        if (!plugin_result.success) {
            plugin_result.lifecycle_event = plugin_result.lifecycle_event.empty()
                ? "evicted"
                : plugin_result.lifecycle_event + "+evicted";
            sessions_.erase(session_key);
        }
    } else {
        const auto result = cli_host_.run(CliRunRequest{
            .spec = ToCliSpec(request.spec),
            .arguments = request.arguments,
            .workspace_path = request.workspace_path,
        });
        plugin_result = PluginRunResult{
            .success = result.success,
            .exit_code = result.exit_code,
            .timed_out = result.timed_out,
            .duration_ms = result.duration_ms,
            .stdout_text = result.stdout_text,
            .stderr_text = result.stderr_text,
            .error_code = result.error_code,
            .error_message = result.error_message,
            .lifecycle_event = "oneshot",
        };
    }

    if (plugin_result.success && request.spec.protocol == "stdio-json-v0" && !IsLikelyJsonObjectString(plugin_result.stdout_text)) {
        return {
            .success = false,
            .exit_code = plugin_result.exit_code,
            .timed_out = plugin_result.timed_out,
            .duration_ms = plugin_result.duration_ms,
            .stdout_text = plugin_result.stdout_text,
            .stderr_text = plugin_result.stderr_text,
            .error_code = "InvalidPluginOutput",
            .error_message = "stdio-json-v0 plugin stdout must be a JSON object",
        };
    }
    if (plugin_result.success && request.spec.protocol == "json-rpc-v0") {
        if (const auto json_rpc_error = JsonRpcOutputError(plugin_result.stdout_text); !json_rpc_error.empty()) {
            return {
                .success = false,
                .exit_code = plugin_result.exit_code,
                .timed_out = plugin_result.timed_out,
                .duration_ms = plugin_result.duration_ms,
                .stdout_text = plugin_result.stdout_text,
                .stderr_text = plugin_result.stderr_text,
                .error_code = "InvalidPluginOutput",
                .error_message = json_rpc_error,
            };
        }
    }
    if (plugin_result.success) {
        const auto plugin_output_json = request.spec.protocol == "json-rpc-v0"
            ? JsonRpcResultObject(plugin_result.stdout_text).value_or("{}")
            : plugin_result.stdout_text;
        if (const auto schema_error = PluginOutputSchemaError(request.spec, plugin_output_json); !schema_error.empty()) {
            return {
                .success = false,
                .exit_code = plugin_result.exit_code,
                .timed_out = plugin_result.timed_out,
                .duration_ms = plugin_result.duration_ms,
                .stdout_text = plugin_result.stdout_text,
                .stderr_text = plugin_result.stderr_text,
                .error_code = "PluginOutputSchemaValidationFailed",
                .error_message = schema_error,
            };
        }
    }

    return plugin_result;
}

}  // namespace agentos
