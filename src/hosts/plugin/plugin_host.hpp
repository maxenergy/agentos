#pragma once

#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentos {

struct PluginSpec {
    std::string manifest_version = "plugin.v1";
    std::string name;
    std::string description;
    std::string binary;
    std::vector<std::string> args_template;
    std::vector<std::string> required_args;
    std::string protocol = "stdio-json-v0";
    std::string input_schema_json = R"({"type":"object"})";
    std::string output_schema_json = R"({"type":"object"})";
    std::string risk_level = "low";
    std::vector<std::string> permissions;
    int timeout_ms = 3000;
    std::size_t output_limit_bytes = 1024 * 1024;
    std::vector<std::string> env_allowlist;
    bool idempotent = true;
    std::size_t memory_limit_bytes = 0;
    int max_processes = 0;
    int cpu_time_limit_seconds = 0;
    int file_descriptor_limit = 0;
    std::vector<std::string> health_args_template;
    int health_timeout_ms = 3000;
    std::string sandbox_mode = "workspace";
    std::string lifecycle_mode = "oneshot";
    int startup_timeout_ms = 3000;
    int idle_timeout_ms = 30000;
    int pool_size = 1;
    std::filesystem::path source_file;
    int source_line_number = 0;
};

bool PluginSpecIsSupported(const PluginSpec& spec);

struct PluginHealthStatus {
    bool supported = false;
    bool command_available = false;
    bool healthy = false;
    std::string reason;
};

PluginHealthStatus CheckPluginHealth(const PluginSpec& spec);
PluginHealthStatus CheckPluginHealth(
    const PluginSpec& spec,
    const CliHost& cli_host,
    const std::filesystem::path& workspace_path);

struct PluginLoadDiagnostic {
    std::filesystem::path file;
    int line_number = 0;
    std::string reason;
};

struct PluginLoadResult {
    std::vector<PluginSpec> specs;
    std::vector<PluginLoadDiagnostic> diagnostics;
};

struct PluginRunRequest {
    PluginSpec spec;
    StringMap arguments;
    std::filesystem::path workspace_path;
};

struct PluginRunResult {
    bool success = false;
    int exit_code = -1;
    bool timed_out = false;
    int duration_ms = 0;
    std::string stdout_text;
    std::string stderr_text;
    std::string error_code;
    std::string error_message;
    std::string lifecycle_event;
};

struct PluginHostOptions {
    std::size_t max_persistent_sessions = 16;
};

struct PluginHostOptionsDiagnostic {
    int line_number = 0;
    std::string reason;
};

struct PluginHostOptionsLoadResult {
    PluginHostOptions options;
    std::vector<PluginHostOptionsDiagnostic> diagnostics;
};

PluginHostOptionsLoadResult LoadPluginHostOptionsWithDiagnostics(const std::filesystem::path& workspace_path);
PluginHostOptions LoadPluginHostOptions(const std::filesystem::path& workspace_path);

struct PluginSessionInfo {
    std::string plugin_name;
    std::string workspace_path;
    std::string binary;
    std::int64_t pid = 0;
    std::int64_t started_at_unix_ms = 0;
    std::int64_t last_used_at_unix_ms = 0;
    int idle_for_ms = 0;
    int idle_timeout_ms = 0;
    int request_count = 0;
    bool alive = false;
    bool idle_expired = false;
};

class PluginHost {
public:
    explicit PluginHost(const CliHost& cli_host, PluginHostOptions options = {});
    ~PluginHost();

    PluginRunResult run(const PluginRunRequest& request) const;
    std::size_t active_session_count() const;
    std::size_t close_all_sessions() const;
    std::size_t close_sessions_for_plugin(const std::string& plugin_name) const;
    std::size_t restart_sessions_for_plugin(const std::string& plugin_name) const;
    std::size_t count_inactive_sessions(const std::string& plugin_name = {}) const;
    std::size_t prune_inactive_sessions(const std::string& plugin_name = {}) const;
    std::vector<PluginSessionInfo> list_sessions() const;

private:
    const CliHost& cli_host_;
    PluginHostOptions options_;
    mutable std::mutex sessions_mutex_;
    mutable std::map<std::string, std::unique_ptr<struct PersistentPluginSession>> sessions_;
};

class PluginSkillInvoker final : public ISkillAdapter {
public:
    PluginSkillInvoker(PluginSpec spec, const PluginHost& plugin_host);

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

private:
    PluginSpec spec_;
    const PluginHost& plugin_host_;
};

std::vector<PluginSpec> LoadPluginSpecsFromDirectory(const std::filesystem::path& spec_dir);
PluginLoadResult LoadPluginSpecsWithDiagnostics(const std::filesystem::path& spec_dir);

}  // namespace agentos
