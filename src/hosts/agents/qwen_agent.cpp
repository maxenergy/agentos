#include "hosts/agents/qwen_agent.hpp"

#include "auth/auth_models.hpp"
#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/cancellation.hpp"
#include "utils/command_utils.hpp"
#include "utils/curl_secret.hpp"
#include "utils/path_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
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
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentos {

namespace {

constexpr char kQwenChatCompletionsUrl[] = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions";
constexpr char kDefaultQwenModel[] = "qwen-plus";
constexpr const char* kCancelledErrorCode = "Cancelled";

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

bool StartsWithCaseInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

bool EmitEvent(const AgentEventCallback& on_event, AgentEvent event) {
    if (!on_event) {
        return true;
    }
    return on_event(std::move(event));
}

// Cap registry listings so a runtime with hundreds of plugin skills
// can't blow up the chat preamble.
constexpr std::size_t kMaxListedQwen = 40;

std::string FormatRegisteredSkillsForChat(const SkillRegistry* registry) {
    if (registry == nullptr) return "  (skill registry unavailable)\n";
    const auto skills = registry->list();
    if (skills.empty()) return "  (none registered)\n";
    std::ostringstream out;
    const auto shown = std::min(skills.size(), kMaxListedQwen);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& m = skills[i];
        out << "  " << m.name
            << " [risk=" << (m.risk_level.empty() ? std::string("unknown") : m.risk_level)
            << ", idempotent=" << (m.idempotent ? "true" : "false") << "]\n";
    }
    if (skills.size() > kMaxListedQwen) {
        out << "  ... and " << (skills.size() - kMaxListedQwen) << " more\n";
    }
    return out.str();
}

std::string FormatRegisteredAgentsForChat(const AgentRegistry* registry) {
    if (registry == nullptr) return "  (agent registry unavailable)\n";
    const auto profiles = registry->list_profiles();
    if (profiles.empty()) return "  (none registered)\n";
    std::ostringstream out;
    const auto shown = std::min(profiles.size(), kMaxListedQwen);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& p = profiles[i];
        out << "  " << p.agent_name
            << " [cost=" << (p.cost_tier.empty() ? std::string("unknown") : p.cost_tier)
            << ", streaming=" << (p.supports_streaming ? "true" : "false") << "]\n";
    }
    if (profiles.size() > kMaxListedQwen) {
        out << "  ... and " << (profiles.size() - kMaxListedQwen) << " more\n";
    }
    return out.str();
}

// Builds the AgentOS-grounded chat preamble used by both BuildPrompt and
// BuildPromptV2. The same wording appears in MainAgent — keep them in
// sync if you reword either.
std::string BuildAgentOsChatPrompt(const std::string& objective,
                                   const SkillRegistry* skill_registry,
                                   const AgentRegistry* agent_registry) {
    const auto skills_text = FormatRegisteredSkillsForChat(skill_registry);
    const auto agents_text = FormatRegisteredAgentsForChat(agent_registry);
    const auto skill_count = skill_registry ? skill_registry->list().size() : 0;
    const auto agent_count = agent_registry ? agent_registry->list_profiles().size() : 0;
    std::ostringstream out;
    out << "You are the AgentOS local runtime assistant. You are running inside a C++ "
           "AgentOS process on the user's own machine — you are NOT a cloud service, "
           "you do not have your own IP, and you should not claim to be ChatGPT, "
           "Qwen-as-cloud-product, or any vendor's hosted chatbot. The user is "
           "interacting with you via an interactive REPL that dispatches free-form "
           "text to you.\n\n"
           "When users ask about your capabilities or skills, refer to the registered "
           "AgentOS skills and agents listed below — these are what you can actually "
           "invoke through this runtime. Do not invent skills you don't see in the "
           "list.\n\n"
           "When users ask about the host machine (IP, hostname, files, network), "
           "explain that you can answer such questions by invoking the appropriate "
           "registered skill (e.g. `host_info` if available), not by guessing.\n\n"
        << "Registered skills (" << skill_count << "):\n"
        << skills_text
        << "\nRegistered agents (" << agent_count << "):\n"
        << agents_text
        << "\nUser: " << objective;
    return out.str();
}

// ---------------------------------------------------------------------------
// Phase 4.5 V2 streaming helper: spawn curl directly so we can consume SSE
// chunks line-by-line and forward them as AgentEvents. CliHost is sync and
// buffers everything until exit, so it cannot be reused for streaming.
// ---------------------------------------------------------------------------

struct StreamProcessResult {
    bool spawn_failed = false;
    bool cancelled = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string error_message;
};

#ifdef _WIN32

std::string BuildCurlCommandLine(const std::string& binary, const std::vector<std::string>& args) {
    std::ostringstream command;
    command << QuoteCommandForDisplay(binary);
    for (const auto& arg : args) {
        command << ' ' << QuoteCommandForDisplay(arg);
    }
    return command.str();
}

bool IsBatchExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return extension == ".cmd" || extension == ".bat";
}

#endif

// Streams curl stdout line-by-line, calling `on_chunk` with each `data:` SSE
// payload. `on_chunk` returns `false` to abort. If the cancellation token is
// set, curl is killed and the function returns promptly.
StreamProcessResult RunCurlStreaming(
    const std::vector<std::string>& args,
    const std::filesystem::path& cwd,
    const std::shared_ptr<CancellationToken>& cancel,
    int timeout_ms,
    const std::function<bool(const std::string& sse_payload)>& on_chunk,
    std::string& tail_buffer) {
    StreamProcessResult result;
    const auto started_at = std::chrono::steady_clock::now();
    const auto deadline_ms = timeout_ms > 0 ? timeout_ms : 120000;

    const auto resolved = ResolveCommandPath("curl");
    if (!resolved.has_value()) {
        result.spawn_failed = true;
        result.error_message = "curl not found on PATH";
        return result;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
        result.spawn_failed = true;
        result.error_message = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    stdin_read = CreateFileA("NUL", GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(STARTUPINFOA);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stdout_write;
    startup_info.hStdInput = stdin_read != INVALID_HANDLE_VALUE ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);

    auto command_line = BuildCurlCommandLine(resolved->string(), args);
    if (IsBatchExtension(*resolved)) {
        command_line = "cmd.exe /d /s /c \"" + command_line + "\"";
    }
    std::string mutable_command_line = command_line;
    auto cwd_string = cwd.string();

    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessA(
        nullptr,
        mutable_command_line.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd_string.c_str(),
        &startup_info,
        &process_info);
    CloseHandle(stdout_write);
    if (stdin_read != INVALID_HANDLE_VALUE) {
        CloseHandle(stdin_read);
    }
    if (!created) {
        CloseHandle(stdout_read);
        result.spawn_failed = true;
        result.error_message = "CreateProcess failed for curl";
        return result;
    }

    // Read in a non-blocking-ish loop: peek before ReadFile so we can poll
    // cancellation/timeout/process exit without hanging on a long-poll SSE.
    std::string buffer;
    bool exit_loop = false;
    bool aborted_for_cancel_or_callback = false;
    while (!exit_loop) {
        if (cancel && cancel->is_cancelled()) {
            result.cancelled = true;
            aborted_for_cancel_or_callback = true;
            break;
        }
        if (ElapsedMs(started_at) > deadline_ms) {
            result.timed_out = true;
            break;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
            // Pipe broken — process likely exited; read the rest then break.
            char tmp[4096];
            DWORD got = 0;
            while (ReadFile(stdout_read, tmp, sizeof(tmp), &got, nullptr) && got > 0) {
                buffer.append(tmp, got);
            }
            break;
        }
        if (available == 0) {
            const DWORD wait_result = WaitForSingleObject(process_info.hProcess, 10);
            if (wait_result == WAIT_OBJECT_0) {
                // Drain remaining data after exit.
                char tmp[4096];
                DWORD got = 0;
                while (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                    if (ReadFile(stdout_read, tmp, sizeof(tmp), &got, nullptr) && got > 0) {
                        buffer.append(tmp, got);
                    } else {
                        break;
                    }
                }
                exit_loop = true;
            }
            continue;
        }

        char tmp[4096];
        DWORD got = 0;
        const DWORD to_read = std::min<DWORD>(available, sizeof(tmp));
        if (!ReadFile(stdout_read, tmp, to_read, &got, nullptr) || got == 0) {
            break;
        }
        buffer.append(tmp, got);

        // Process complete lines.
        std::size_t line_end;
        while ((line_end = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, line_end);
            buffer.erase(0, line_end + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (line.rfind("data:", 0) != 0) {
                continue;
            }
            std::string payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') {
                payload.erase(0, 1);
            }
            if (!on_chunk(payload)) {
                aborted_for_cancel_or_callback = true;
                exit_loop = true;
                break;
            }
        }
    }

    if (aborted_for_cancel_or_callback || result.timed_out) {
        TerminateProcess(process_info.hProcess, 1);
    }
    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(stdout_read);

    tail_buffer = buffer;
    return result;

#else
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        result.spawn_failed = true;
        result.error_message = std::string("pipe() failed: ") + std::strerror(errno);
        return result;
    }
    int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags != -1) {
        fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    std::vector<std::string> argv_strings;
    argv_strings.reserve(args.size() + 1);
    argv_strings.push_back("curl");
    for (const auto& a : args) {
        argv_strings.push_back(a);
    }
    std::vector<char*> argv;
    argv.reserve(argv_strings.size() + 1);
    for (auto& s : argv_strings) {
        argv.push_back(s.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid == -1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        result.spawn_failed = true;
        result.error_message = std::string("fork() failed: ") + std::strerror(errno);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        if (chdir(cwd.string().c_str()) != 0) {
            _exit(126);
        }
        execvp(resolved->string().c_str(), argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipe_fds[1]);

    std::string buffer;
    bool exit_loop = false;
    bool aborted_for_cancel_or_callback = false;
    while (!exit_loop) {
        if (cancel && cancel->is_cancelled()) {
            result.cancelled = true;
            aborted_for_cancel_or_callback = true;
            break;
        }
        if (ElapsedMs(started_at) > deadline_ms) {
            result.timed_out = true;
            break;
        }

        char tmp[4096];
        const auto bytes_read = read(pipe_fds[0], tmp, sizeof(tmp));
        if (bytes_read > 0) {
            buffer.append(tmp, bytes_read);

            std::size_t line_end;
            while ((line_end = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, line_end);
                buffer.erase(0, line_end + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }
                if (line.rfind("data:", 0) != 0) {
                    continue;
                }
                std::string payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') {
                    payload.erase(0, 1);
                }
                if (!on_chunk(payload)) {
                    aborted_for_cancel_or_callback = true;
                    exit_loop = true;
                    break;
                }
            }
            continue;
        }
        if (bytes_read == 0) {
            // EOF — child closed the pipe.
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = 128 + WTERMSIG(status);
            }
            close(pipe_fds[0]);
            tail_buffer = buffer;
            return result;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Poll for child exit (non-blocking) before sleeping.
            int status = 0;
            const pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                // Drain anything left in the pipe.
                while (true) {
                    const auto leftover = read(pipe_fds[0], tmp, sizeof(tmp));
                    if (leftover <= 0) {
                        break;
                    }
                    buffer.append(tmp, leftover);
                }
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = 128 + WTERMSIG(status);
                }
                close(pipe_fds[0]);
                tail_buffer = buffer;
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // Other read error — bail.
        break;
    }

    if (aborted_for_cancel_or_callback || result.timed_out) {
        kill(-pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    close(pipe_fds[0]);
    tail_buffer = buffer;
    return result;
#endif
}

}  // namespace

QwenAgent::QwenAgent(
    const CliHost& cli_host,
    const CredentialBroker& credential_broker,
    const AuthProfileStore& profile_store,
    std::filesystem::path workspace_root,
    const SkillRegistry* skill_registry,
    const AgentRegistry* agent_registry)
    : cli_host_(cli_host),
      credential_broker_(credential_broker),
      profile_store_(profile_store),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))),
      skill_registry_(skill_registry),
      agent_registry_(agent_registry) {}

AgentProfile QwenAgent::profile() const {
    return {
        .agent_name = "qwen",
        .version = "0.1.0",
        .description = "Adapter for Alibaba Cloud Model Studio Qwen through the authenticated model provider layer.",
        .capabilities = {
            {"analysis", 85},
            {"planning", 80},
            {"code_reasoning", 85},
        },
        .supports_session = false,
        // Phase 4.5: V2 path streams DashScope OpenAI-compatible SSE.
        .supports_streaming = true,
        .supports_patch = false,
        .supports_subagents = false,
        .supports_network = true,
        .cost_tier = "provider-billed",
        .latency_tier = "medium",
        .risk_level = "medium",
    };
}

bool QwenAgent::healthy() const {
    const auto session = credential_broker_.get_session(AuthProviderId::qwen, profile_name(std::nullopt));
    return session.has_value() && (CommandExists("curl") || CommandExists("curl.exe"));
}

std::string QwenAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "qwen-session-" + std::to_string(next_id);
}

std::optional<std::string> QwenAgent::open_session(const StringMap& config) {
    (void)config;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return std::string("qwen-session-") + std::to_string(next_id);
}

void QwenAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult QwenAgent::run_task(const AgentTask& task) {
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

    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "curl was not found on PATH",
        };
    }

    const auto profile = profile_name(task.auth_profile);
    const auto session = credential_broker_.get_session(AuthProviderId::qwen, profile);
    if (!session.has_value()) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = "qwen auth session is unavailable for profile " + profile,
        };
    }

    std::string token;
    try {
        token = credential_broker_.get_access_token(AuthProviderId::qwen, profile);
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = error.what(),
        };
    }

    const auto model = model_name(task);
    const auto body = BuildRequestBody(task);

    // Stage the request body and the `Authorization: Bearer ...` header to
    // short-lived files so the Qwen API key never appears in argv (where it
    // would be visible via /proc/<pid>/cmdline or the Windows process list).
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(
            workspace_path,
            body,
            /*header_lines=*/{"Authorization: Bearer " + token});
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = std::string("could not stage qwen request: ") + error.what(),
        };
    }
    const auto body_arg = std::string("@") + secret_files.body_file.string();
    const auto headers_arg = std::string("@") + secret_files.headers_file.string();

    const CliSpec spec{
        .name = "qwen_chat_completions",
        .description = "Call Alibaba Cloud Model Studio OpenAI-compatible Chat Completions.",
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
            {"url", kQwenChatCompletionsUrl},
            // Retain `api_key` so the redactor can still scrub the token from
            // command_display / stdout / stderr even though it is no longer
            // expanded into argv.
            {"api_key", token},
            {"auth_headers_file", headers_arg},
            {"request_body_file", body_arg},
            {"max_time_seconds", std::to_string(std::max(1, spec.timeout_ms / 1000))},
        },
        .workspace_path = workspace_path,
    });

    const auto extracted_text = ExtractFirstMessageContent(result.stdout_text);
    const auto summary = result.success ? (extracted_text.empty() ? result.stdout_text : extracted_text) : "";
    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "qwen";
    legacy_output_json["provider"] = "qwen";
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
        .summary = result.success ? summary : "Qwen chat completions request failed.",
        .structured_output_json = legacy_output,
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "qwen",
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

AgentResult QwenAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool QwenAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

// ---------------------------------------------------------------------------
// V2 invoke path: streams DashScope OpenAI-compatible SSE through curl spawned
// directly. Falls back to the legacy non-streaming path on stream failure or
// when no `on_event` callback is supplied.
// ---------------------------------------------------------------------------
AgentResult QwenAgent::invoke(const AgentInvocation& invocation,
                              const AgentEventCallback& on_event) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto cancelled = [&]() {
        return invocation.cancel && invocation.cancel->is_cancelled();
    };

    auto fallback_to_sync = [&]() {
        auto sync_task = InvocationToTask(invocation);
        auto result = run_task(sync_task);
        result.from_stream_fallback = true;
        return result;
    };

    if (cancelled()) {
        return AgentResult{
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = kCancelledErrorCode,
            .error_message = "qwen invoke cancelled before request",
        };
    }

    // Sync mode (no callback) → reuse legacy path verbatim. `from_stream_fallback`
    // stays false because the caller never asked for streaming.
    if (!on_event) {
        auto sync_task = InvocationToTask(invocation);
        return run_task(sync_task);
    }

    // Workspace + auth checks mirror run_task. Failures fall back to sync so
    // the orchestrator gets one consistent error path.
    const auto workspace_path = NormalizeWorkspaceRoot(
        invocation.workspace_path.empty() ? workspace_root_ : invocation.workspace_path);
    std::error_code workspace_ec;
    if (!std::filesystem::exists(workspace_path, workspace_ec) ||
        !std::filesystem::is_directory(workspace_path, workspace_ec)) {
        return fallback_to_sync();
    }
    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return fallback_to_sync();
    }
    const auto profile = profile_name(invocation.auth_profile);
    const auto session = credential_broker_.get_session(AuthProviderId::qwen, profile);
    if (!session.has_value()) {
        return fallback_to_sync();
    }
    std::string token;
    try {
        token = credential_broker_.get_access_token(AuthProviderId::qwen, profile);
    } catch (const std::exception&) {
        return fallback_to_sync();
    }

    const auto body = BuildRequestBodyV2(invocation, /*stream=*/true);
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(
            workspace_path,
            body,
            /*header_lines=*/{
                "Authorization: Bearer " + token,
                "Content-Type: application/json",
                "Accept: text/event-stream",
            });
    } catch (const std::exception&) {
        return fallback_to_sync();
    }
    const auto body_arg = std::string("@") + secret_files.body_file.string();
    const auto headers_arg = std::string("@") + secret_files.headers_file.string();

    const int timeout_ms = invocation.timeout_ms > 0 ? invocation.timeout_ms : 120000;
    const auto max_time_seconds = std::to_string(std::max(1, timeout_ms / 1000));

    const std::vector<std::string> curl_args{
        "-L",
        "--silent",
        "--show-error",
        "--fail-with-body",
        "--no-buffer",
        "--max-time",
        max_time_seconds,
        kQwenChatCompletionsUrl,
        "-H",
        headers_arg,
        "-X",
        "POST",
        "-d",
        body_arg,
    };

    AgentUsage usage;
    std::string accumulated_text;
    std::string session_id_seen;
    std::string model_seen;
    bool saw_done = false;
    bool callback_aborted = false;

    auto handle_chunk = [&](const std::string& payload) -> bool {
        if (cancelled()) {
            return false;
        }
        // Treat stripped-down "[DONE]" as the SSE terminator.
        std::string trimmed = payload;
        while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' ')) {
            trimmed.pop_back();
        }
        if (trimmed == "[DONE]") {
            saw_done = true;
            return true;
        }
        nlohmann::json chunk;
        try {
            chunk = nlohmann::json::parse(trimmed);
        } catch (const nlohmann::json::exception&) {
            // Ignore unparseable lines (keep-alive comments etc.)
            return true;
        }

        // First chunk: id + model → SessionInit.
        if (session_id_seen.empty() && chunk.contains("id") && chunk["id"].is_string()) {
            session_id_seen = chunk["id"].get<std::string>();
            if (chunk.contains("model") && chunk["model"].is_string()) {
                model_seen = chunk["model"].get<std::string>();
            }
            AgentEvent ev{};
            ev.kind = AgentEvent::Kind::SessionInit;
            ev.fields["session_id"] = session_id_seen;
            if (!model_seen.empty()) {
                ev.fields["model"] = model_seen;
            }
            ev.fields["agent"] = "qwen";
            if (!EmitEvent(on_event, std::move(ev))) {
                callback_aborted = true;
                return false;
            }
        } else if (model_seen.empty() && chunk.contains("model") && chunk["model"].is_string()) {
            model_seen = chunk["model"].get<std::string>();
        }

        if (chunk.contains("choices") && chunk["choices"].is_array() && !chunk["choices"].empty()) {
            const auto& choice = chunk["choices"][0];
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& delta = choice["delta"];
                if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                    const auto thinking = delta["reasoning_content"].get<std::string>();
                    if (!thinking.empty()) {
                        AgentEvent ev{};
                        ev.kind = AgentEvent::Kind::Thinking;
                        ev.payload_text = thinking;
                        if (!EmitEvent(on_event, std::move(ev))) {
                            callback_aborted = true;
                            return false;
                        }
                    }
                }
                if (delta.contains("content") && delta["content"].is_string()) {
                    const auto piece = delta["content"].get<std::string>();
                    if (!piece.empty()) {
                        accumulated_text += piece;
                        AgentEvent ev{};
                        ev.kind = AgentEvent::Kind::TextDelta;
                        ev.payload_text = piece;
                        if (!EmitEvent(on_event, std::move(ev))) {
                            callback_aborted = true;
                            return false;
                        }
                    }
                }
            }
        }

        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            const auto& u = chunk["usage"];
            if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer()) {
                usage.input_tokens = u["prompt_tokens"].get<int>();
            }
            if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer()) {
                usage.output_tokens = u["completion_tokens"].get<int>();
            }
            usage.turns = 1;
            if (!model_seen.empty()) {
                usage.per_model[model_seen] = std::to_string(usage.input_tokens + usage.output_tokens);
            }
            AgentEvent ev{};
            ev.kind = AgentEvent::Kind::Usage;
            ev.fields["input_tokens"] = std::to_string(usage.input_tokens);
            ev.fields["output_tokens"] = std::to_string(usage.output_tokens);
            ev.fields["cost_usd"] = "0.0";
            ev.fields["turns"] = "1";
            if (!EmitEvent(on_event, std::move(ev))) {
                callback_aborted = true;
                return false;
            }
        }
        return true;
    };

    std::string tail;
    const auto stream_outcome = RunCurlStreaming(
        curl_args,
        workspace_path,
        invocation.cancel,
        timeout_ms,
        handle_chunk,
        tail);

    // Cancellation is a terminal state: do not fall back to sync.
    if (stream_outcome.cancelled || cancelled()) {
        AgentEvent ev{};
        ev.kind = AgentEvent::Kind::Error;
        ev.fields["error_code"] = kCancelledErrorCode;
        ev.fields["error_message"] = "qwen invoke cancelled";
        EmitEvent(on_event, std::move(ev));
        return AgentResult{
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = kCancelledErrorCode,
            .error_message = "qwen invoke cancelled",
        };
    }

    // Spawn failure / non-zero curl exit / no chunks → sync fallback.
    const bool stream_failed =
        stream_outcome.spawn_failed ||
        stream_outcome.timed_out ||
        stream_outcome.exit_code != 0 ||
        (!saw_done && session_id_seen.empty() && accumulated_text.empty() && !callback_aborted);
    if (stream_failed) {
        AgentEvent ev{};
        ev.kind = AgentEvent::Kind::Status;
        ev.fields["phase"] = "stream_failed";
        ev.payload_text = "qwen streaming failed; falling back to sync";
        EmitEvent(on_event, std::move(ev));
        return fallback_to_sync();
    }

    // Emit terminal Final event — orchestrator stops consuming events on this.
    AgentEvent final_ev{};
    final_ev.kind = AgentEvent::Kind::Final;
    final_ev.fields["success"] = "true";
    final_ev.payload_text = accumulated_text;
    EmitEvent(on_event, std::move(final_ev));

    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "qwen";
    legacy_output_json["provider"] = "qwen";
    legacy_output_json["profile"] = profile;
    legacy_output_json["model"] = model_seen.empty() ? model_name_v2(invocation) : model_seen;
    legacy_output_json["content"] = accumulated_text;
    legacy_output_json["session_id"] = session_id_seen;
    legacy_output_json["streamed"] = true;
    const auto legacy_output = legacy_output_json.dump();
    AgentResult agent_result{
        .success = true,
        .summary = accumulated_text,
        .structured_output_json = legacy_output,
        .duration_ms = ElapsedMs(started_at),
        .estimated_cost = 0.0,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "qwen",
        .success = true,
        .summary = accumulated_text,
        .structured_output_json = legacy_output,
        .artifacts = {},
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = "",
        .error_message = "",
    });
    agent_result.usage = usage;
    if (!session_id_seen.empty()) {
        agent_result.session_id = session_id_seen;
    }
    agent_result.from_stream_fallback = false;
    return agent_result;
}

std::string QwenAgent::profile_name(const std::optional<std::string>& requested_profile) const {
    return requested_profile.value_or(profile_store_.default_profile(AuthProviderId::qwen).value_or("default"));
}

std::string QwenAgent::model_name(const AgentTask& task) {
    const auto marker = task.constraints_json.find("\"model\":\"");
    if (marker != std::string::npos) {
        const auto start = marker + 9;
        const auto end = task.constraints_json.find('"', start);
        if (end != std::string::npos && end > start) {
            return task.constraints_json.substr(start, end - start);
        }
    }
    return kDefaultQwenModel;
}

std::string QwenAgent::model_name_v2(const AgentInvocation& invocation) {
    const auto it = invocation.constraints.find("model");
    if (it != invocation.constraints.end() && !it->second.empty()) {
        return it->second;
    }
    return kDefaultQwenModel;
}

std::string QwenAgent::BuildPrompt(const AgentTask& task) const {
    if (StartsWithCaseInsensitive(task.objective, "return exactly:")) {
        return task.objective;
    }
    if (task.task_type == "chat") {
        return BuildAgentOsChatPrompt(task.objective, skill_registry_, agent_registry_);
    }

    std::ostringstream prompt;
    prompt
        << "You are running as a model provider agent inside AgentOS.\n"
        << "Return a concise, useful answer for the requested task.\n"
        << "If the objective asks for an exact phrase or exact output, return only that exact content.\n\n"
        << "Task id: " << task.task_id << "\n"
        << "Task type: " << task.task_type << "\n"
        << "Objective: " << task.objective << "\n"
        << "Workspace: " << task.workspace_path << "\n";

    if (!task.context_json.empty()) {
        prompt << "Context JSON: " << task.context_json << "\n";
    }
    if (!task.constraints_json.empty()) {
        prompt << "Constraints JSON: " << task.constraints_json << "\n";
    }

    return prompt.str();
}

std::string QwenAgent::BuildPromptV2(const AgentInvocation& invocation) const {
    if (StartsWithCaseInsensitive(invocation.objective, "return exactly:")) {
        return invocation.objective;
    }
    if (const auto it = invocation.context.find("task_type");
        it != invocation.context.end() && it->second == "chat") {
        return BuildAgentOsChatPrompt(invocation.objective, skill_registry_, agent_registry_);
    }
    std::ostringstream prompt;
    prompt
        << "You are running as a model provider agent inside AgentOS.\n"
        << "Return a concise, useful answer for the requested task.\n"
        << "If the objective asks for an exact phrase or exact output, return only that exact content.\n\n"
        << "Task id: " << invocation.task_id << "\n"
        << "Objective: " << invocation.objective << "\n"
        << "Workspace: " << invocation.workspace_path.string() << "\n";

    auto type_it = invocation.context.find("task_type");
    if (type_it != invocation.context.end()) {
        prompt << "Task type: " << type_it->second << "\n";
    }
    return prompt.str();
}

std::string QwenAgent::BuildRequestBody(const AgentTask& task) const {
    nlohmann::ordered_json message;
    message["role"] = "user";
    message["content"] = BuildPrompt(task);

    nlohmann::ordered_json body;
    body["model"] = model_name(task);
    body["messages"] = nlohmann::ordered_json::array({message});
    return body.dump();
}

std::string QwenAgent::BuildRequestBodyV2(const AgentInvocation& invocation, bool stream) const {
    nlohmann::ordered_json message;
    message["role"] = "user";
    message["content"] = BuildPromptV2(invocation);

    nlohmann::ordered_json body;
    body["model"] = model_name_v2(invocation);
    body["messages"] = nlohmann::ordered_json::array({message});
    body["stream"] = stream;
    return body.dump();
}

std::string QwenAgent::ExtractFirstMessageContent(const std::string& response_json) {
    try {
        const auto parsed = nlohmann::json::parse(response_json);
        const auto choices = parsed.find("choices");
        if (choices == parsed.end() || !choices->is_array()) {
            return "";
        }
        for (const auto& choice : *choices) {
            if (!choice.is_object()) {
                continue;
            }
            const auto message = choice.find("message");
            if (message == choice.end() || !message->is_object()) {
                continue;
            }
            const auto content = message->find("content");
            if (content != message->end() && content->is_string()) {
                return content->get<std::string>();
            }
        }
    } catch (const nlohmann::json::exception&) {
        return "";
    }
    return "";
}

AgentTask QwenAgent::InvocationToTask(const AgentInvocation& invocation) {
    // Project the V2 invocation back onto the legacy task shape so run_task
    // can do its existing thing without behavior drift.
    AgentTask task;
    task.task_id = invocation.task_id;
    auto type_it = invocation.context.find("task_type");
    task.task_type = type_it == invocation.context.end() ? std::string{} : type_it->second;
    task.objective = invocation.objective;
    task.workspace_path = invocation.workspace_path.string();
    task.auth_profile = invocation.auth_profile;

    // Encode context as a flat JSON object so BuildPrompt's "Context JSON:"
    // section continues to surface orchestrator-provided fields (task_type,
    // parent_task_id, role, ...) when the V2 invoke path falls back to
    // run_task in sync mode.
    if (!invocation.context.empty()) {
        nlohmann::ordered_json context_json;
        for (const auto& [k, v] : invocation.context) {
            context_json[k] = v;
        }
        task.context_json = context_json.dump();
    }

    // Encode constraints as a flat JSON object so the legacy model_name parser
    // continues to find `"model":"..."` if the caller passed one.
    if (!invocation.constraints.empty()) {
        nlohmann::ordered_json constraints_json;
        for (const auto& [k, v] : invocation.constraints) {
            constraints_json[k] = v;
        }
        task.constraints_json = constraints_json.dump();
    }
    task.timeout_ms = invocation.timeout_ms;
    task.budget_limit = invocation.budget_limit_usd;
    return task;
}

}  // namespace agentos
