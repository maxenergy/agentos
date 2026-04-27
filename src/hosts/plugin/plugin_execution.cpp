#include "hosts/plugin/plugin_host.hpp"

#include "core/policy/permission_model.hpp"
#include "hosts/plugin/plugin_json_rpc.hpp"
#include "hosts/plugin/plugin_persistent_session.hpp"
#include "hosts/plugin/plugin_sandbox.hpp"
#include "hosts/plugin/plugin_schema_validator.hpp"
#include "hosts/plugin/plugin_spec_utils.hpp"
#include "utils/command_utils.hpp"
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

std::size_t PluginHost::close_sessions_for_plugin(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::size_t closed = 0;
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        if (iterator->second && iterator->second->spec.name == plugin_name) {
            iterator = sessions_.erase(iterator);
            ++closed;
        } else {
            ++iterator;
        }
    }
    return closed;
}

std::size_t PluginHost::restart_sessions_for_plugin(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::size_t restarted = 0;
    for (auto& [key, session] : sessions_) {
        (void)key;
        if (!session || session->spec.name != plugin_name) {
            continue;
        }
        const auto spec = session->spec;
        const auto workspace_path = session->workspace_path;
        session.reset();
        std::string start_error;
        auto fresh = PersistentPluginSession::start(spec, workspace_path, start_error);
        if (fresh && fresh->wait_until_started(spec.startup_timeout_ms)) {
            session = std::move(fresh);
            ++restarted;
        }
    }
    return restarted;
}

std::size_t PluginHost::count_inactive_sessions(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::size_t inactive = 0;
    for (const auto& [key, session] : sessions_) {
        (void)key;
        if (!session) {
            if (plugin_name.empty()) {
                ++inactive;
            }
            continue;
        }
        if (!plugin_name.empty() && session->spec.name != plugin_name) {
            continue;
        }
        if (!session->alive() || session->idle_expired(session->spec.idle_timeout_ms)) {
            ++inactive;
        }
    }
    return inactive;
}

std::size_t PluginHost::prune_inactive_sessions(const std::string& plugin_name) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::size_t pruned = 0;
    for (auto iterator = sessions_.begin(); iterator != sessions_.end();) {
        auto& session = iterator->second;
        if (!session) {
            if (plugin_name.empty()) {
                iterator = sessions_.erase(iterator);
                ++pruned;
            } else {
                ++iterator;
            }
            continue;
        }
        if (!plugin_name.empty() && session->spec.name != plugin_name) {
            ++iterator;
            continue;
        }
        if (!session->alive() || session->idle_expired(session->spec.idle_timeout_ms)) {
            iterator = sessions_.erase(iterator);
            ++pruned;
        } else {
            ++iterator;
        }
    }
    return pruned;
}

std::vector<PluginSessionInfo> PluginHost::list_sessions() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<PluginSessionInfo> entries;
    entries.reserve(sessions_.size());
    const auto now_steady = std::chrono::steady_clock::now();
    for (const auto& [key, session] : sessions_) {
        (void)key;
        if (!session) {
            continue;
        }
        PluginSessionInfo info;
        info.plugin_name = session->spec.name;
        info.workspace_path = session->workspace_path.string();
        info.binary = session->spec.binary;
        info.pid = session->pid_value();
        info.started_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            session->started_at_wall.time_since_epoch()).count();
        info.last_used_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            session->last_used_at_wall.time_since_epoch()).count();
        info.idle_for_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            now_steady - session->last_used_at).count());
        info.idle_timeout_ms = session->spec.idle_timeout_ms;
        info.request_count = session->request_count;
        info.alive = session->alive();
        info.idle_expired = info.alive && session->idle_expired(session->spec.idle_timeout_ms);
        entries.push_back(std::move(info));
    }
    return entries;
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

void EvictOldestSessionForPlugin(
    std::map<std::string, std::unique_ptr<PersistentPluginSession>>& sessions,
    const std::string& plugin_name,
    const std::string& requested_session_key,
    const int pool_size) {
    if (pool_size <= 0 || sessions.contains(requested_session_key)) {
        return;
    }
    PruneInactivePersistentSessions(sessions);
    while (true) {
        std::size_t plugin_session_count = 0;
        for (const auto& [key, session] : sessions) {
            (void)key;
            if (session && session->spec.name == plugin_name) {
                ++plugin_session_count;
            }
        }
        if (plugin_session_count < static_cast<std::size_t>(pool_size)) {
            break;
        }
        auto oldest = sessions.end();
        for (auto iterator = sessions.begin(); iterator != sessions.end(); ++iterator) {
            if (iterator->first == requested_session_key || !iterator->second) {
                continue;
            }
            if (iterator->second->spec.name != plugin_name) {
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
        const int effective_pool_size = (std::max)(1, request.spec.pool_size);
        const int capped_pool_size = options_.max_persistent_sessions == 0
            ? effective_pool_size
            : (std::min)(effective_pool_size, static_cast<int>(options_.max_persistent_sessions));
        EvictOldestSessionForPlugin(sessions_, request.spec.name, session_key, capped_pool_size);
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
            session.reset();
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
