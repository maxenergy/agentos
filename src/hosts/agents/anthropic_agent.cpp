#include "hosts/agents/anthropic_agent.hpp"

#include "auth/auth_models.hpp"
#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/cancellation.hpp"
#include "utils/command_utils.hpp"
#include "utils/curl_secret.hpp"
#include "utils/path_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentos {

namespace {

constexpr char kAnthropicMessagesUrl[] = "https://api.anthropic.com/v1/messages";
constexpr char kAnthropicVersion[] = "2023-06-01";
constexpr char kDefaultClaudeModel[] = "claude-3-5-sonnet-20240620";

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

bool StringTrue(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::optional<std::string> TaskContextValue(const AgentTask& task, const std::string& key) {
    if (task.context_json.empty()) {
        return std::nullopt;
    }
    try {
        const auto context = nlohmann::json::parse(task.context_json);
        if (context.is_object() && context.contains(key) && context[key].is_string()) {
            return context[key].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

bool AllowsWorkspaceWrites(const AgentTask& task) {
    const auto value = TaskContextValue(task, "allow_writes");
    return value.has_value() && StringTrue(*value);
}

bool AllowsWorkspaceWrites(const AgentInvocation& invocation) {
    const auto it = invocation.context.find("allow_writes");
    return it != invocation.context.end() && StringTrue(it->second);
}

std::string ReadEnvVarLocal(const std::string& name) {
    if (name.empty()) {
        return {};
    }
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name.c_str()) == 0 && raw != nullptr) {
        std::string value(raw, size > 0 ? size - 1 : 0);
        std::free(raw);
        return value;
    }
    return {};
#else
    if (const char* raw = std::getenv(name.c_str()); raw != nullptr) {
        return raw;
    }
    return {};
#endif
}

std::string ResolveClaudePermissionMode(const bool allow_writes) {
    const auto configured = ReadEnvVarLocal("AGENTOS_CLAUDE_PERMISSION_MODE");
    if (!configured.empty()) {
        return configured;
    }
    return allow_writes ? "bypassPermissions" : "";
}

// ---- SSE streaming subprocess helpers (Phase 4.4) ------------------------
//
// We can't go through CliHost for the streaming path because CliHost drains
// the entire stdout into a single string before returning. Anthropic's
// streaming Messages endpoint emits an unbounded SSE event stream that we
// need to parse incrementally so the orchestrator can apply admission
// control and cancel mid-flight.
//
// The runner below spawns curl directly, exposes stdout as a line callback,
// and polls a CancellationToken so the process can be killed promptly when
// the orchestrator (or the on_event callback returning false) asks us to
// stop.

struct StreamProcessResult {
    bool spawned = false;          // false if the binary could not be spawned at all
    int exit_code = -1;            // process exit code (or signal+128 on POSIX)
    bool cancelled = false;        // true if we killed it because of CancellationToken
    bool callback_stopped = false; // true if on_event returned false and we killed it
};

// Process one buffered SSE block delimited by an empty line. Returns false
// if the line callback signaled cancellation.
using SseLineCallback = std::function<bool(std::string)>;

void FlushSseLines(std::string& buffer, const SseLineCallback& on_line, bool& should_stop) {
    while (!should_stop) {
        const auto newline = buffer.find('\n');
        if (newline == std::string::npos) {
            return;
        }
        std::string line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!on_line(std::move(line))) {
            should_stop = true;
            return;
        }
    }
}

#ifdef _WIN32

StreamProcessResult RunCurlStreamingProcess(
    const std::filesystem::path& binary,
    const std::vector<std::string>& args,
    const std::filesystem::path& cwd,
    const std::shared_ptr<CancellationToken>& cancel,
    int timeout_ms,
    const SseLineCallback& on_line) {
    StreamProcessResult result;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(SECURITY_ATTRIBUTES);
    security.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0)) {
        return result;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE stdin_read = CreateFileA(
        "NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    // Build the same lpCommandLine string CreateProcess expects, quoting
    // arguments that contain spaces. We reuse cli_host's quoting helper.
    std::ostringstream cmd;
    cmd << QuoteCommandForDisplay(binary.string());
    for (const auto& arg : args) {
        cmd << ' ' << QuoteCommandForDisplay(arg);
    }
    std::string cmd_line = cmd.str();

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.hStdInput = stdin_read != INVALID_HANDLE_VALUE ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    auto cwd_string = cwd.string();
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    const BOOL created = CreateProcessA(
        nullptr,
        cmd_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr,
        cwd_string.c_str(),
        &si,
        &pi);

    CloseHandle(stdout_write);
    if (stdin_read != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_read);
    }

    if (!created) {
        CloseHandle(stdout_read);
        if (job) CloseHandle(job);
        return result;
    }

    if (job) {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);

    result.spawned = true;

    // Reader thread: read stdout, accumulate buffer, and dispatch SSE lines
    // through on_line. Sets `should_stop` if on_line returns false.
    std::string buffer;
    std::atomic<bool> reader_done{false};
    std::atomic<bool> should_stop{false};
    std::thread reader([&]() {
        constexpr DWORD kBufferSize = 4096;
        char buf[kBufferSize];
        DWORD bytes_read = 0;
        while (true) {
            BOOL ok = ReadFile(stdout_read, buf, kBufferSize, &bytes_read, nullptr);
            if (!ok || bytes_read == 0) {
                break;
            }
            buffer.append(buf, bytes_read);
            bool stop_now = false;
            FlushSseLines(buffer, on_line, stop_now);
            if (stop_now) {
                should_stop.store(true);
                break;
            }
        }
        reader_done.store(true);
    });

    // Watchdog loop: poll for cancellation / callback stop / timeout.
    const auto started_at = std::chrono::steady_clock::now();
    bool killed = false;
    while (true) {
        DWORD wait_result = WaitForSingleObject(pi.hProcess, 50);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (cancel && cancel->is_cancelled()) {
            result.cancelled = true;
            killed = true;
            break;
        }
        if (should_stop.load()) {
            result.callback_stopped = true;
            killed = true;
            break;
        }
        if (timeout_ms > 0 && ElapsedMs(started_at) >= timeout_ms) {
            killed = true;
            break;
        }
    }

    if (killed) {
        if (job) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
        WaitForSingleObject(pi.hProcess, 2000);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);

    if (reader.joinable()) {
        reader.join();
    }

    // Drain any trailing complete lines.
    if (!buffer.empty()) {
        bool stop_now = false;
        FlushSseLines(buffer, on_line, stop_now);
    }

    CloseHandle(stdout_read);
    return result;
}

#else

StreamProcessResult RunCurlStreamingProcess(
    const std::filesystem::path& binary,
    const std::vector<std::string>& args,
    const std::filesystem::path& cwd,
    const std::shared_ptr<CancellationToken>& cancel,
    int timeout_ms,
    const SseLineCallback& on_line) {
    StreamProcessResult result;

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return result;
    }

    std::vector<std::string> argv_storage;
    argv_storage.push_back(binary.string());
    for (const auto& arg : args) {
        argv_storage.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t child_pid = fork();
    if (child_pid == -1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return result;
    }

    if (child_pid == 0) {
        setpgid(0, 0);
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        if (const int dev_null = open("/dev/null", O_RDONLY); dev_null != -1) {
            dup2(dev_null, STDIN_FILENO);
            close(dev_null);
        }
        if (chdir(cwd.string().c_str()) != 0) {
            _exit(126);
        }
        execvp(binary.string().c_str(), argv.data());
        _exit(127);
    }

    setpgid(child_pid, child_pid);
    close(pipe_fds[1]);
    result.spawned = true;

    // Make the read end non-blocking so the watchdog loop can break out of
    // read() when it needs to kill the process.
    const int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags != -1) {
        fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::string buffer;
    bool should_stop = false;
    bool killed = false;
    const auto started_at = std::chrono::steady_clock::now();

    while (true) {
        char buf[4096];
        const auto bytes_read = read(pipe_fds[0], buf, sizeof(buf));
        if (bytes_read > 0) {
            buffer.append(buf, static_cast<std::size_t>(bytes_read));
            FlushSseLines(buffer, on_line, should_stop);
            if (should_stop) {
                result.callback_stopped = true;
                killed = true;
                break;
            }
            continue;
        }

        if (bytes_read == 0) {
            // EOF: child closed its stdout. waitpid below.
            break;
        }

        // bytes_read == -1
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }

        // Poll status & cancellation while no data is ready.
        int status = 0;
        const pid_t r = waitpid(child_pid, &status, WNOHANG);
        if (r == child_pid) {
            // Child exited. Drain remaining output.
            while (true) {
                const auto more = read(pipe_fds[0], buf, sizeof(buf));
                if (more > 0) {
                    buffer.append(buf, static_cast<std::size_t>(more));
                    FlushSseLines(buffer, on_line, should_stop);
                    if (should_stop) {
                        result.callback_stopped = true;
                        break;
                    }
                    continue;
                }
                break;
            }
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = 128 + WTERMSIG(status);
            }
            close(pipe_fds[0]);
            return result;
        }

        if (cancel && cancel->is_cancelled()) {
            result.cancelled = true;
            killed = true;
            break;
        }
        if (timeout_ms > 0 && ElapsedMs(started_at) >= timeout_ms) {
            killed = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (killed) {
        kill(-child_pid, SIGKILL);
    }

    int status = 0;
    waitpid(child_pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }

    // Drain any tail bytes that arrived between EAGAIN and the kill.
    while (true) {
        char buf[4096];
        const auto more = read(pipe_fds[0], buf, sizeof(buf));
        if (more > 0) {
            buffer.append(buf, static_cast<std::size_t>(more));
            bool drop = false;
            FlushSseLines(buffer, on_line, drop);
            continue;
        }
        break;
    }

    close(pipe_fds[0]);
    return result;
}

#endif

// SSE event accumulator. Anthropic emits W3C SSE blocks like:
//
//   event: message_start
//   data: {"type":"message_start","message":{"id":"...","model":"...",
//                                            "usage":{"input_tokens":10}}}
//
//   event: content_block_delta
//   data: {"type":"content_block_delta","index":0,
//          "delta":{"type":"text_delta","text":"Hello"}}
//
//   event: message_stop
//   data: {"type":"message_stop"}
//
// Empty line delimits one event block. The event name must be picked up from
// the most recent `event:` line so multi-data lines map to it correctly.
//
// We also handle thinking deltas (`delta.type == "thinking_delta"` carrying
// `delta.thinking`) per Anthropic extended-thinking docs.
struct SseEventBuffer {
    std::string event_name;
    std::string data_json;

    void reset() {
        event_name.clear();
        data_json.clear();
    }
};

}  // namespace

AnthropicAgent::AnthropicAgent(
    const CliHost& cli_host,
    const CredentialBroker& credential_broker,
    const AuthProfileStore& profile_store,
    std::filesystem::path workspace_root)
    : cli_host_(cli_host),
      credential_broker_(credential_broker),
      profile_store_(profile_store),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))) {}

AgentProfile AnthropicAgent::profile() const {
    return {
        .agent_name = "anthropic",
        .version = "0.1.0",
        .description = "Adapter for Anthropic Claude through the authenticated model provider layer.",
        .capabilities = {
            {"analysis", 95},
            {"planning", 90},
            {"code_reasoning", 95},
        },
        .supports_session = false,
        .supports_streaming = true,
        .supports_patch = false,
        .supports_subagents = false,
        .supports_network = true,
        .cost_tier = "provider-billed",
        .latency_tier = "medium",
        .risk_level = "medium",
    };
}

bool AnthropicAgent::healthy() const {
    const auto session = credential_broker_.get_session(AuthProviderId::anthropic, profile_name(std::nullopt));
    if (!session.has_value()) {
        return false;
    }
    if (session->managed_by_external_cli || session->access_token_ref == "external-cli:claude") {
        return CommandExists("claude");
    }
    return CommandExists("curl") || CommandExists("curl.exe");
}

std::string AnthropicAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "anthropic-session-" + std::to_string(next_id);
}

void AnthropicAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult AnthropicAgent::run_task(const AgentTask& task) {
    const auto started_at = std::chrono::steady_clock::now();

    const auto requested_workspace = task.workspace_path.empty()
        ? workspace_root_
        : std::filesystem::path(task.workspace_path);
    const auto workspace_path = NormalizeWorkspaceRoot(requested_workspace);
    std::error_code workspace_ec;
    if (!std::filesystem::exists(workspace_path, workspace_ec) ||
        !std::filesystem::is_directory(workspace_path, workspace_ec)) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "InvalidWorkspace",
            .error_message = "agent workspace must be an existing directory",
        };
    }

    const auto profile = profile_name(task.auth_profile);
    const auto session = credential_broker_.get_session(AuthProviderId::anthropic, profile);
    if (!session.has_value()) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = "anthropic auth session is unavailable for profile " + profile,
        };
    }

    if (session->managed_by_external_cli || session->access_token_ref == "external-cli:claude") {
        return run_task_with_cli_session(task, *session, workspace_path);
    }

    return run_task_with_rest_session(task, *session, workspace_path);
}

AgentResult AnthropicAgent::run_task_with_rest_session(
    const AgentTask& task,
    const AuthSession& session,
    const std::filesystem::path& workspace_path) {
    (void)session;
    const auto started_at = std::chrono::steady_clock::now();

    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "curl was not found on PATH",
        };
    }

    const auto profile = profile_name(task.auth_profile);
    std::string token;
    try {
        token = credential_broker_.get_access_token(AuthProviderId::anthropic, profile);
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = error.what(),
        };
    }

    const auto model = model_name(task);
    const auto url = std::string(kAnthropicMessagesUrl);
    const auto body = BuildRequestBody(task);

    // Stage the request body and the `x-api-key` / version headers to short-
    // lived files so the Anthropic API key never appears on the curl process
    // command line (where it would be visible via /proc/<pid>/cmdline or the
    // Windows process listing).
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(
            workspace_path,
            body,
            /*header_lines=*/{
                "x-api-key: " + token,
                std::string("anthropic-version: ") + kAnthropicVersion,
            });
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = std::string("could not stage anthropic request: ") + error.what(),
        };
    }
    const auto body_arg = std::string("@") + secret_files.body_file.string();
    const auto headers_arg = std::string("@") + secret_files.headers_file.string();

    const CliSpec spec{
        .name = "anthropic_messages",
        .description = "Call the Anthropic Messages REST endpoint.",
        .binary = "curl",
        .args_template = {
            "-L",
            "--silent",
            "--show-error",
            "--fail-with-body",
            "--max-time",
            "{{max_time_seconds}}",
            "{{url}}",
            "-H",
            "Content-Type: application/json",
            "-H",
            "{{auth_headers_file}}",
            "-X",
            "POST",
            "-d",
            "{{request_body_file}}",
        },
        .required_args = {"url", "auth_headers_file", "request_body_file", "max_time_seconds"},
        .input_schema_json = R"({"type":"object","required":["url","request_body_file"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "json",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : 120000,
        .output_limit_bytes = 1024 * 1024,
    };

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"url", url},
            // Retain `api_key` for SecretRedactor even though it is no longer
            // expanded into argv — it scrubs the token from command_display
            // and any echoed stdout/stderr.
            {"api_key", token},
            {"auth_headers_file", headers_arg},
            {"request_body_file", body_arg},
            {"max_time_seconds", std::to_string(std::max(1, spec.timeout_ms / 1000))},
        },
        .workspace_path = workspace_path,
    });

    const auto extracted_text = ExtractFirstTextPart(result.stdout_text);
    const auto summary = result.success ? (extracted_text.empty() ? result.stdout_text : extracted_text) : "";
    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "anthropic";
    legacy_output_json["provider"] = "anthropic";
    legacy_output_json["profile"] = profile;
    legacy_output_json["model"] = model;
    legacy_output_json["content"] = summary;
    legacy_output_json["command"] = result.command_display;
    legacy_output_json["exit_code"] = result.exit_code;
    legacy_output_json["response"] = result.stdout_text;
    legacy_output_json["stderr"] = result.stderr_text;
    const auto legacy_output = legacy_output_json.dump();
    AgentResult agent_result{
        .success = result.success,
        .summary = result.success ? summary : "Anthropic messages request failed.",
        .structured_output_json = legacy_output,
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "anthropic",
        .success = agent_result.success,
        .summary = agent_result.summary,
        .structured_output_json = legacy_output,
        .artifacts = agent_result.artifacts,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = agent_result.estimated_cost,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    });
    return agent_result;
}

AgentResult AnthropicAgent::run_task_with_cli_session(
    const AgentTask& task,
    const AuthSession& session,
    const std::filesystem::path& workspace_path) {
    const auto started_at = std::chrono::steady_clock::now();

    if (!CommandExists("claude")) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "claude CLI was not found on PATH",
        };
    }

    const auto model = model_name(task);
    const bool allow_writes = AllowsWorkspaceWrites(task);
    const auto permission_mode = ResolveClaudePermissionMode(allow_writes);
    const CliSpec spec{
        .name = "anthropic_cli_agent",
        .description = "Run Claude Code in non-interactive print mode using its external session.",
        .binary = "claude",
        .args_template = {
            "--print",
            "--output-format",
            "text",
            "{{permission_mode_flag}}",
            "{{permission_mode}}",
            "{{prompt}}",
        },
        .required_args = {"prompt", "permission_mode_flag", "permission_mode"},
        .input_schema_json = R"({"type":"object","required":["prompt"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = allow_writes
            ? std::vector<std::string>{"process.spawn", "network.access", "filesystem.write", "filesystem.read"}
            : std::vector<std::string>{"process.spawn", "network.access", "filesystem.read"},
        .timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : 120000,
        .output_limit_bytes = 1024 * 1024,
        .env_allowlist = {
            "USERPROFILE",
            "HOMEDRIVE",
            "HOMEPATH",
            "HOME",
            "APPDATA",
            "LOCALAPPDATA",
            "XDG_CONFIG_HOME",
            "CLAUDE_CONFIG_DIR",
        },
    };

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"prompt", BuildPrompt(task)},
            {"permission_mode_flag", !permission_mode.empty() ? "--permission-mode" : ""},
            {"permission_mode", permission_mode},
        },
        .workspace_path = workspace_path,
    });

    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "anthropic";
    legacy_output_json["provider"] = "anthropic";
    legacy_output_json["profile"] = session.profile_name;
    legacy_output_json["auth_source"] = "claude_cli_session";
    legacy_output_json["model"] = model;
    legacy_output_json["content"] = result.success ? result.stdout_text : "";
    legacy_output_json["command"] = result.command_display;
    legacy_output_json["exit_code"] = result.exit_code;
    legacy_output_json["stdout"] = result.stdout_text;
    legacy_output_json["stderr"] = result.stderr_text;
    const auto legacy_output = legacy_output_json.dump();
    AgentResult agent_result{
        .success = result.success,
        .summary = result.success ? result.stdout_text : "Claude CLI task failed.",
        .structured_output_json = legacy_output,
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "anthropic",
        .success = agent_result.success,
        .summary = agent_result.summary,
        .structured_output_json = legacy_output,
        .artifacts = agent_result.artifacts,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = agent_result.estimated_cost,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    });
    return agent_result;
}

AgentResult AnthropicAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool AnthropicAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

// ---- IAgentAdapterV2 ----------------------------------------------------

AgentResult AnthropicAgent::invoke(
    const AgentInvocation& invocation,
    const AgentEventCallback& on_event) {
    const auto started_at = std::chrono::steady_clock::now();

    const auto workspace_path = NormalizeWorkspaceRoot(
        invocation.workspace_path.empty() ? workspace_root_ : invocation.workspace_path);
    std::error_code workspace_ec;
    if (!std::filesystem::exists(workspace_path, workspace_ec) ||
        !std::filesystem::is_directory(workspace_path, workspace_ec)) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "InvalidWorkspace",
            .error_message = "agent workspace must be an existing directory",
        };
    }

    if (invocation.cancel && invocation.cancel->is_cancelled()) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "Cancelled",
            .error_message = "invocation was cancelled before dispatch",
        };
    }

    const auto profile = profile_name(invocation.auth_profile);
    const auto session = credential_broker_.get_session(AuthProviderId::anthropic, profile);
    if (!session.has_value()) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = "anthropic auth session is unavailable for profile " + profile,
        };
    }

    // External CLI sessions don't support our SSE wire protocol; fall back to
    // the legacy CLI dispatch and tag the result so the orchestrator knows it
    // came from the non-streaming path.
    if (session->managed_by_external_cli || session->access_token_ref == "external-cli:claude") {
        auto result = run_task_with_cli_session(TaskFromInvocation(invocation), *session, workspace_path);
        result.from_stream_fallback = true;
        return result;
    }

    // Try streaming first when a callback was supplied. Without a callback we
    // skip straight to the synchronous REST path — there is no point spawning
    // a streaming process whose events nobody is listening to.
    if (on_event) {
        if (auto streamed = invoke_with_rest_streaming(invocation, *session, workspace_path, on_event)) {
            return std::move(*streamed);
        }
    }

    auto fallback = run_task_with_rest_session(TaskFromInvocation(invocation), *session, workspace_path);
    if (on_event) {
        // We only set from_stream_fallback when streaming was actually
        // attempted and failed — a sync-only invoke (no on_event) is just the
        // legacy REST path with a different entry point.
        fallback.from_stream_fallback = true;
    }
    return fallback;
}

std::optional<AgentResult> AnthropicAgent::invoke_with_rest_streaming(
    const AgentInvocation& invocation,
    const AuthSession& session,
    const std::filesystem::path& workspace_path,
    const AgentEventCallback& on_event) {
    (void)session;
    const auto started_at = std::chrono::steady_clock::now();

    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return std::nullopt;
    }

    const auto profile = profile_name(invocation.auth_profile);
    std::string token;
    try {
        token = credential_broker_.get_access_token(AuthProviderId::anthropic, profile);
    } catch (const std::exception&) {
        return std::nullopt;
    }

    const auto body = BuildRequestBodyV2(invocation, /*stream=*/true);

    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(
            workspace_path,
            body,
            /*header_lines=*/{
                "x-api-key: " + token,
                std::string("anthropic-version: ") + kAnthropicVersion,
                "accept: text/event-stream",
            });
    } catch (const std::exception&) {
        return std::nullopt;
    }

    const auto body_arg = std::string("@") + secret_files.body_file.string();
    const auto headers_arg = std::string("@") + secret_files.headers_file.string();

    const int timeout_ms = invocation.timeout_ms > 0 ? invocation.timeout_ms : 120000;
    const std::string max_time_seconds = std::to_string(std::max(1, timeout_ms / 1000));

    // Mirror the args used by the non-streaming path, minus --silent so curl
    // forwards SSE chunks promptly. The token still lives in the headers
    // file (Phase 1.1).
    std::vector<std::string> args = {
        "-N",                  // disable curl's output buffering
        "--no-buffer",         // belt and suspenders for older curl builds
        "--show-error",
        "--max-time", max_time_seconds,
        kAnthropicMessagesUrl,
        "-H", "Content-Type: application/json",
        "-H", headers_arg,
        "-X", "POST",
        "-d", body_arg,
    };

    const auto curl_path = ResolveCommandPath("curl").value_or(std::filesystem::path("curl"));

    // Streaming state.
    SseEventBuffer current_event;
    AgentUsage usage;
    std::optional<std::string> session_id;
    std::string accumulated_text;
    std::string final_model;
    bool saw_message_stop = false;
    bool saw_session_init = false;
    std::string error_code;
    std::string error_message;
    bool callback_requested_stop = false;

    auto emit_session_init = [&](const nlohmann::json& message) {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::SessionInit;
        if (message.contains("id") && message["id"].is_string()) {
            session_id = message["id"].get<std::string>();
            ev.fields["session_id"] = *session_id;
        }
        if (message.contains("model") && message["model"].is_string()) {
            final_model = message["model"].get<std::string>();
            ev.fields["model"] = final_model;
        }
        ev.fields["version"] = kAnthropicVersion;
        if (!on_event(ev)) {
            callback_requested_stop = true;
        }
        saw_session_init = true;
    };

    auto emit_usage = [&]() {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Usage;
        ev.fields["input_tokens"] = std::to_string(usage.input_tokens);
        ev.fields["output_tokens"] = std::to_string(usage.output_tokens);
        ev.fields["cost_usd"] = std::to_string(usage.cost_usd);
        if (!on_event(ev)) {
            callback_requested_stop = true;
        }
    };

    auto handle_event_block = [&]() {
        if (current_event.event_name.empty() || current_event.data_json.empty()) {
            current_event.reset();
            return;
        }
        // Anthropic uses JSON in `data:` lines. Tolerate parse failures
        // silently — they typically come from curl's progress meter or stray
        // lines and shouldn't kill the stream.
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(current_event.data_json);
        } catch (const nlohmann::json::exception&) {
            current_event.reset();
            return;
        }

        const auto& name = current_event.event_name;
        if (name == "message_start" && parsed.contains("message") && parsed["message"].is_object()) {
            const auto& message = parsed["message"];
            emit_session_init(message);
            if (message.contains("usage") && message["usage"].is_object()) {
                const auto& u = message["usage"];
                if (u.contains("input_tokens") && u["input_tokens"].is_number_integer()) {
                    usage.input_tokens = u["input_tokens"].get<int>();
                }
                if (u.contains("output_tokens") && u["output_tokens"].is_number_integer()) {
                    usage.output_tokens = u["output_tokens"].get<int>();
                }
                emit_usage();
            }
        } else if (name == "content_block_delta" && parsed.contains("delta") && parsed["delta"].is_object()) {
            const auto& delta = parsed["delta"];
            const auto delta_type = delta.value("type", std::string{});
            if (delta_type == "text_delta" || delta.contains("text")) {
                if (delta.contains("text") && delta["text"].is_string()) {
                    AgentEvent ev;
                    ev.kind = AgentEvent::Kind::TextDelta;
                    ev.payload_text = delta["text"].get<std::string>();
                    accumulated_text += ev.payload_text;
                    if (!on_event(ev)) {
                        callback_requested_stop = true;
                    }
                }
            } else if (delta_type == "thinking_delta" || delta.contains("thinking")) {
                if (delta.contains("thinking") && delta["thinking"].is_string()) {
                    AgentEvent ev;
                    ev.kind = AgentEvent::Kind::Thinking;
                    ev.payload_text = delta["thinking"].get<std::string>();
                    if (!on_event(ev)) {
                        callback_requested_stop = true;
                    }
                }
            }
        } else if (name == "message_delta") {
            if (parsed.contains("usage") && parsed["usage"].is_object()) {
                const auto& u = parsed["usage"];
                if (u.contains("output_tokens") && u["output_tokens"].is_number_integer()) {
                    usage.output_tokens = u["output_tokens"].get<int>();
                }
                if (u.contains("input_tokens") && u["input_tokens"].is_number_integer()) {
                    usage.input_tokens = u["input_tokens"].get<int>();
                }
                emit_usage();
            }
        } else if (name == "message_stop") {
            saw_message_stop = true;
            AgentEvent ev;
            ev.kind = AgentEvent::Kind::Final;
            if (!on_event(ev)) {
                callback_requested_stop = true;
            }
        } else if (name == "error") {
            error_code = "ExternalProcessFailed";
            if (parsed.contains("error") && parsed["error"].is_object()) {
                const auto& err = parsed["error"];
                error_message = err.value("message", std::string("anthropic stream error"));
                error_code = err.value("type", error_code);
            } else {
                error_message = "anthropic stream error";
            }
            AgentEvent ev;
            ev.kind = AgentEvent::Kind::Error;
            ev.fields["error_code"] = error_code;
            ev.fields["error_message"] = error_message;
            if (!on_event(ev)) {
                callback_requested_stop = true;
            }
        }
        current_event.reset();
    };

    auto on_line = [&](std::string line) -> bool {
        if (line.empty()) {
            handle_event_block();
            return !callback_requested_stop;
        }
        if (line.rfind("event:", 0) == 0) {
            std::string value = line.substr(6);
            // strip leading whitespace
            std::size_t start = value.find_first_not_of(" \t");
            current_event.event_name = (start == std::string::npos) ? std::string() : value.substr(start);
        } else if (line.rfind("data:", 0) == 0) {
            std::string value = line.substr(5);
            std::size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) {
                if (!current_event.data_json.empty()) {
                    current_event.data_json.push_back('\n');
                }
                current_event.data_json.append(value, start);
            }
        }
        // Ignore comments (':' lines) and unknown fields.
        return !callback_requested_stop;
    };

    const auto stream_result = RunCurlStreamingProcess(
        curl_path,
        args,
        workspace_path,
        invocation.cancel,
        timeout_ms,
        on_line);

    if (!stream_result.spawned) {
        return std::nullopt;
    }

    // Ensure any partial trailing event is flushed.
    if (!current_event.event_name.empty() || !current_event.data_json.empty()) {
        handle_event_block();
    }

    // Cancellation takes precedence over everything else: even if the
    // process happened to exit cleanly between our cancel signal and the
    // wait, surface Cancelled so the caller doesn't treat it as success.
    if (stream_result.cancelled || (invocation.cancel && invocation.cancel->is_cancelled())) {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Error;
        ev.fields["error_code"] = "Cancelled";
        ev.fields["error_message"] = "invocation was cancelled";
        on_event(ev);
        return AgentResult{
            .success = false,
            .summary = "",
            .structured_output_json = "{}",
            .duration_ms = ElapsedMs(started_at),
            .error_code = "Cancelled",
            .error_message = "invocation was cancelled",
            .usage = usage,
            .session_id = session_id,
            .from_stream_fallback = false,
        };
    }

    const bool stream_succeeded =
        stream_result.exit_code == 0 && saw_message_stop && saw_session_init && error_code.empty();

    if (!stream_succeeded) {
        // Streaming attempt failed — let the caller fall back to the
        // synchronous REST path.
        return std::nullopt;
    }

    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "anthropic";
    legacy_output_json["provider"] = "anthropic";
    legacy_output_json["profile"] = profile;
    legacy_output_json["model"] = final_model.empty() ? ModelNameFromConstraints(invocation.constraints) : final_model;
    legacy_output_json["content"] = accumulated_text;
    legacy_output_json["stream"] = true;
    legacy_output_json["input_tokens"] = usage.input_tokens;
    legacy_output_json["output_tokens"] = usage.output_tokens;
    const auto legacy_output = legacy_output_json.dump();
    AgentResult agent_result{
        .success = true,
        .summary = accumulated_text,
        .structured_output_json = legacy_output,
        .duration_ms = ElapsedMs(started_at),
        .estimated_cost = usage.cost_usd,
        .usage = usage,
        .session_id = session_id,
        .from_stream_fallback = false,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "anthropic",
        .success = agent_result.success,
        .summary = agent_result.summary,
        .structured_output_json = legacy_output,
        .artifacts = agent_result.artifacts,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = agent_result.estimated_cost,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    });
    return agent_result;
}

std::string AnthropicAgent::profile_name(const std::optional<std::string>& requested_profile) const {
    return requested_profile.value_or(profile_store_.default_profile(AuthProviderId::anthropic).value_or("default"));
}

std::string AnthropicAgent::model_name(const AgentTask& task) {
    const auto marker = task.constraints_json.find("\"model\":\"");
    if (marker != std::string::npos) {
        const auto start = marker + 9;
        const auto end = task.constraints_json.find('"', start);
        if (end != std::string::npos && end > start) {
            return task.constraints_json.substr(start, end - start);
        }
    }
    return kDefaultClaudeModel;
}

std::string AnthropicAgent::ModelNameFromConstraints(const StringMap& constraints) {
    const auto it = constraints.find("model");
    if (it != constraints.end() && !it->second.empty()) {
        return it->second;
    }
    return kDefaultClaudeModel;
}

std::string AnthropicAgent::BuildPrompt(const AgentTask& task) {
    // Chat mode skips the agent-orchestration preamble so a user message
    // reaches the model verbatim instead of being wrapped in scaffolding.
    if (task.task_type == "chat") {
        return "You are a helpful chat assistant. Reply naturally and concisely.\n\nUser: " + task.objective;
    }

    std::ostringstream prompt;
    if (AllowsWorkspaceWrites(task)) {
        prompt
            << "Implement this user request now by editing files in the workspace.\n\n"
            << "USER REQUEST:\n"
            << task.objective << "\n\n"
            << "REQUIREMENTS:\n"
            << "- Do not only acknowledge the request.\n"
            << "- Do not stop after explaining your role.\n"
            << "- Create or modify the files needed to satisfy the request.\n"
            << "- If the user did not specify an exact file, choose a reasonable path in this workspace.\n"
            << "- At the end, list changed files and how to build or run the result.\n\n";
        if (const auto contract = TaskContextValue(task, "contract_instructions");
            contract.has_value() && !contract->empty()) {
            prompt << "ACCEPTANCE CONTRACT:\n" << *contract << "\n";
            if (const auto manifest = TaskContextValue(task, "deliverables_manifest");
                manifest.has_value() && !manifest->empty()) {
                prompt << "Write the deliverables manifest exactly here: " << *manifest << "\n"
                       << "Manifest JSON schema:\n"
                       << "{\n"
                       << "  \"status\": \"complete|partial|blocked\",\n"
                       << "  \"deliverables\": [\n"
                       << "    {\"path\": \"workspace-relative-or-absolute\", \"type\": \"inferred\", "
                          "\"description\": \"what was delivered\", \"verification\": \"passed|not_applicable|failed\"}\n"
                       << "  ],\n"
                       << "  \"verification\": [{\"command\": \"command run or manual check\", \"success\": true, "
                          "\"notes\": \"result\"}],\n"
                       << "  \"blockers\": []\n"
                       << "}\n";
            }
            prompt << "\n";
        }
    } else {
        prompt
            << "You are running as a model provider agent inside AgentOS.\n"
            << "Return a concise, useful answer for the requested task.\n"
            << "If the objective asks for an exact phrase or exact output, return only that exact content.\n\n"
            << "Objective: " << task.objective << "\n";
    }

    prompt
        << "Task id: " << task.task_id << "\n"
        << "Task type: " << task.task_type << "\n"
        << "Workspace: " << task.workspace_path << "\n";

    if (!task.context_json.empty()) {
        prompt << "Context JSON: " << task.context_json << "\n";
    }
    if (!task.constraints_json.empty()) {
        prompt << "Constraints JSON: " << task.constraints_json << "\n";
    }

    return prompt.str();
}

std::string AnthropicAgent::BuildPromptV2(const AgentInvocation& invocation) {
    if (const auto it = invocation.context.find("task_type");
        it != invocation.context.end() && it->second == "chat") {
        return "You are a helpful chat assistant. Reply naturally and concisely.\n\nUser: " + invocation.objective;
    }

    std::ostringstream prompt;
    if (AllowsWorkspaceWrites(invocation)) {
        prompt
            << "Implement this user request now by editing files in the workspace.\n\n"
            << "USER REQUEST:\n"
            << invocation.objective << "\n\n"
            << "REQUIREMENTS:\n"
            << "- Do not only acknowledge the request.\n"
            << "- Do not stop after explaining your role.\n"
            << "- Create or modify the files needed to satisfy the request.\n"
            << "- If the user did not specify an exact file, choose a reasonable path in this workspace.\n"
            << "- At the end, list changed files and how to build or run the result.\n\n";
        const auto contract = invocation.context.find("contract_instructions");
        if (contract != invocation.context.end() && !contract->second.empty()) {
            prompt << "ACCEPTANCE CONTRACT:\n" << contract->second << "\n";
            const auto manifest = invocation.context.find("deliverables_manifest");
            if (manifest != invocation.context.end() && !manifest->second.empty()) {
                prompt << "Write the deliverables manifest exactly here: " << manifest->second << "\n"
                       << "Manifest JSON schema:\n"
                       << "{\n"
                       << "  \"status\": \"complete|partial|blocked\",\n"
                       << "  \"deliverables\": [\n"
                       << "    {\"path\": \"workspace-relative-or-absolute\", \"type\": \"inferred\", "
                          "\"description\": \"what was delivered\", \"verification\": \"passed|not_applicable|failed\"}\n"
                       << "  ],\n"
                       << "  \"verification\": [{\"command\": \"command run or manual check\", \"success\": true, "
                          "\"notes\": \"result\"}],\n"
                       << "  \"blockers\": []\n"
                       << "}\n";
            }
            prompt << "\n";
        }
    } else {
        prompt
            << "You are running as a model provider agent inside AgentOS.\n"
            << "Return a concise, useful answer for the requested task.\n"
            << "If the objective asks for an exact phrase or exact output, return only that exact content.\n\n"
            << "Objective: " << invocation.objective << "\n";
    }

    prompt
        << "Task id: " << invocation.task_id << "\n"
        << "Workspace: " << invocation.workspace_path.string() << "\n";

    if (!invocation.context.empty()) {
        prompt << "Context:\n";
        for (const auto& [k, v] : invocation.context) {
            prompt << "  " << k << ": " << v << "\n";
        }
    }
    if (!invocation.constraints.empty()) {
        prompt << "Constraints:\n";
        for (const auto& [k, v] : invocation.constraints) {
            prompt << "  " << k << ": " << v << "\n";
        }
    }

    return prompt.str();
}

std::string AnthropicAgent::BuildRequestBody(const AgentTask& task) {
    nlohmann::ordered_json body;
    body["model"] = model_name(task);
    body["max_tokens"] = 4096;
    body["messages"] = nlohmann::ordered_json::array({
        {{"role", "user"}, {"content", BuildPrompt(task)}},
    });
    return body.dump();
}

std::string AnthropicAgent::BuildRequestBodyV2(const AgentInvocation& invocation, bool stream) {
    nlohmann::json body;
    body["model"] = ModelNameFromConstraints(invocation.constraints);
    body["max_tokens"] = 4096;
    body["stream"] = stream;
    body["messages"] = nlohmann::json::array({
        {{"role", "user"}, {"content", BuildPromptV2(invocation)}},
    });
    return body.dump();
}

std::string AnthropicAgent::ExtractFirstTextPart(const std::string& response_json) {
    try {
        const auto parsed = nlohmann::json::parse(response_json);
        const auto content = parsed.find("content");
        if (content == parsed.end() || !content->is_array()) {
            return "";
        }
        for (const auto& part : *content) {
            const auto text = part.find("text");
            if (text != part.end() && text->is_string()) {
                return text->get<std::string>();
            }
        }
    } catch (const nlohmann::json::exception&) {
        return "";
    }
    return "";
}

AgentTask AnthropicAgent::TaskFromInvocation(const AgentInvocation& invocation) {
    // Pack structured constraints back into the legacy constraints_json blob
    // so model_name() and BuildPrompt() keep working without duplication.
    nlohmann::json context_json = nlohmann::json::object();
    for (const auto& [k, v] : invocation.context) {
        context_json[k] = v;
    }
    nlohmann::json constraints_json = nlohmann::json::object();
    for (const auto& [k, v] : invocation.constraints) {
        constraints_json[k] = v;
    }
    AgentTask task;
    task.task_id = invocation.task_id;
    task.task_type = "agent_invoke";
    task.objective = invocation.objective;
    task.workspace_path = invocation.workspace_path.string();
    task.auth_profile = invocation.auth_profile;
    task.context_json = context_json.empty() ? std::string() : context_json.dump();
    task.constraints_json = constraints_json.empty() ? std::string() : constraints_json.dump();
    task.timeout_ms = invocation.timeout_ms;
    task.budget_limit = invocation.budget_limit_usd;
    return task;
}

}  // namespace agentos
