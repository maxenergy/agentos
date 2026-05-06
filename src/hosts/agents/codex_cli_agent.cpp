#include "hosts/agents/codex_cli_agent.hpp"

#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/cancellation.hpp"
#include "utils/command_utils.hpp"
#include "utils/path_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace agentos {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return "";
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

// ----- Streaming process runner (V2 path) -----------------------------------
//
// CliHost runs to completion before returning, so it cannot drive a streaming
// NDJSON pipeline or be cancelled mid-flight. We spawn `codex` directly here
// and feed each stdout line back to the caller as we read it.

struct StreamProcessResult {
    bool launched = false;
    int exit_code = -1;
    bool timed_out = false;
    bool cancelled = false;
    int duration_ms = 0;
    std::string command_display;
    std::string stderr_text;
    std::string error_code;
    std::string error_message;
};

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at)
            .count());
}

std::string QuoteArgForDisplay(const std::string& value) {
    if (!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '/' || c == '\\' || c == ':' || c == '=';
        })) {
        return value;
    }
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

std::string BuildCommandDisplay(const std::string& binary, const std::vector<std::string>& args) {
    std::string out = QuoteArgForDisplay(binary);
    for (const auto& arg : args) {
        out.push_back(' ');
        out.append(QuoteArgForDisplay(arg));
    }
    return out;
}

#ifdef _WIN32

std::string BuildWindowsCommandLine(const std::string& binary, const std::vector<std::string>& args) {
    auto quote = [](const std::string& s) -> std::string {
        if (!s.empty() && s.find_first_of(" \t\n\v\"") == std::string::npos) {
            return s;
        }
        std::string r = "\"";
        std::size_t backslashes = 0;
        for (char ch : s) {
            if (ch == '\\') {
                ++backslashes;
            } else if (ch == '"') {
                r.append(backslashes * 2 + 1, '\\');
                r.push_back('"');
                backslashes = 0;
            } else {
                r.append(backslashes, '\\');
                backslashes = 0;
                r.push_back(ch);
            }
        }
        r.append(backslashes * 2, '\\');
        r.push_back('"');
        return r;
    };

    std::string cmd = quote(binary);
    for (const auto& arg : args) {
        cmd.push_back(' ');
        cmd.append(quote(arg));
    }
    return cmd;
}

#endif

bool StringTrue(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

bool AllowsWorkspaceWrites(const AgentTask& task) {
    if (task.context_json.empty()) {
        return false;
    }
    try {
        const auto context = nlohmann::json::parse(task.context_json);
        if (context.is_object() && context.contains("allow_writes") &&
            context["allow_writes"].is_string()) {
            return StringTrue(context["allow_writes"].get<std::string>());
        }
    } catch (const nlohmann::json::exception&) {
    }
    return false;
}

bool AllowsWorkspaceWrites(const AgentInvocation& invocation) {
    const auto it = invocation.context.find("allow_writes");
    return it != invocation.context.end() && StringTrue(it->second);
}

// Splits the running stdout buffer into complete lines and dispatches each to
// `on_line`. Leaves any trailing partial line in `buffer` for the next chunk.
void DrainLines(std::string& buffer, const std::function<void(const std::string&)>& on_line) {
    std::size_t start = 0;
    while (true) {
        const auto newline = buffer.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }
        std::string line = buffer.substr(start, newline - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        on_line(line);
        start = newline + 1;
    }
    if (start > 0) {
        buffer.erase(0, start);
    }
}

// Runs `binary` with `args` from `cwd`, dispatching each NDJSON line on stdout
// to `on_line`. Polls the cancellation token and the deadline; kills the
// process tree when either fires. `on_line` returning false also kills.
StreamProcessResult RunStreamingProcess(
    const std::string& binary,
    const std::vector<std::string>& args,
    const std::string& stdin_text,
    const std::filesystem::path& cwd,
    int timeout_ms,
    const std::shared_ptr<CancellationToken>& cancel,
    const std::function<bool(const std::string&)>& on_line) {
    const auto started_at = std::chrono::steady_clock::now();
    StreamProcessResult result;
    result.command_display = BuildCommandDisplay(binary, args);

    const auto resolved = ResolveCommandPath(binary);
    if (!resolved.has_value()) {
        result.error_code = "AgentUnavailable";
        result.error_message = "binary not found on PATH: " + binary;
        result.duration_ms = ElapsedMs(started_at);
        return result;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
        !CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
        result.error_code = "PipeCreateFailed";
        result.error_message = "failed to create stdio pipes";
        if (stdout_read) CloseHandle(stdout_read);
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_read) CloseHandle(stderr_read);
        if (stderr_write) CloseHandle(stderr_write);
        if (stdin_read) CloseHandle(stdin_read);
        if (stdin_write) CloseHandle(stdin_write);
        result.duration_ms = ElapsedMs(started_at);
        return result;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = stdin_read;

    PROCESS_INFORMATION pi{};
    auto cmdline = BuildWindowsCommandLine(resolved->string(), args);
    auto cwd_str = cwd.string();

    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job == nullptr) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_read);
        CloseHandle(stderr_write);
        CloseHandle(stdin_read);
        CloseHandle(stdin_write);
        result.error_code = "ResourceLimitSetupFailed";
        result.error_message = "failed to create job object";
        result.duration_ms = ElapsedMs(started_at);
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    BOOL created = CreateProcessA(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr,
        cwd_str.c_str(),
        &si,
        &pi);

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    CloseHandle(stdin_read);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        CloseHandle(stdin_write);
        CloseHandle(job);
        result.error_code = "ProcessStartFailed";
        result.error_message = "CreateProcess failed";
        result.duration_ms = ElapsedMs(started_at);
        return result;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    result.launched = true;
    if (!stdin_text.empty()) {
        DWORD written = 0;
        WriteFile(stdin_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written, nullptr);
    }
    CloseHandle(stdin_write);

    // stderr drained on a worker thread (small).
    std::string stderr_buf;
    std::thread stderr_thread([stderr_read, &stderr_buf]() {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(stderr_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
            if (stderr_buf.size() < 64 * 1024) {
                stderr_buf.append(buf, n);
            }
        }
    });

    std::string stdout_buf;
    std::atomic<bool> stop{false};
    bool callback_aborted = false;
    auto deadline_reached = [&]() {
        if (timeout_ms <= 0) return false;
        return ElapsedMs(started_at) >= timeout_ms;
    };

    while (true) {
        if (cancel && cancel->is_cancelled()) {
            result.cancelled = true;
            stop = true;
        }
        if (deadline_reached()) {
            result.timed_out = true;
            stop = true;
        }
        if (stop) {
            TerminateJobObject(job, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }

        DWORD bytes_avail = 0;
        if (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
            char buf[4096];
            DWORD n = 0;
            if (ReadFile(stdout_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
                stdout_buf.append(buf, n);
                DrainLines(stdout_buf, [&](const std::string& line) {
                    if (callback_aborted) return;
                    if (!on_line(line)) {
                        callback_aborted = true;
                        stop = true;
                    }
                });
                continue;
            }
        }

        DWORD wait_status = WaitForSingleObject(pi.hProcess, 25);
        if (wait_status == WAIT_OBJECT_0) {
            // Drain remaining bytes.
            char buf[4096];
            DWORD n = 0;
            while (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0 &&
                   ReadFile(stdout_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
                stdout_buf.append(buf, n);
            }
            DrainLines(stdout_buf, [&](const std::string& line) {
                if (callback_aborted) return;
                if (!on_line(line)) {
                    callback_aborted = true;
                }
            });
            if (!stdout_buf.empty()) {
                if (!callback_aborted) {
                    on_line(stdout_buf);
                }
                stdout_buf.clear();
            }
            break;
        }
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    CloseHandle(job);

    if (stderr_thread.joinable()) {
        stderr_thread.join();
    }
    result.stderr_text = std::move(stderr_buf);
    if (callback_aborted && !result.cancelled) {
        result.cancelled = true;
    }
#else
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0 || pipe(in_pipe) != 0) {
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        result.error_code = "PipeCreateFailed";
        result.error_message = "failed to create stdio pipes";
        result.duration_ms = ElapsedMs(started_at);
        return result;
    }

    auto set_nonblock = [](int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    };
    set_nonblock(out_pipe[0]);
    set_nonblock(err_pipe[0]);

    std::vector<std::string> argv_owned;
    argv_owned.reserve(args.size() + 1);
    argv_owned.push_back(resolved->string());
    for (const auto& a : args) argv_owned.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argv_owned) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        result.error_code = "ProcessStartFailed";
        result.error_message = std::string("fork failed: ") + std::strerror(errno);
        result.duration_ms = ElapsedMs(started_at);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (chdir(cwd.string().c_str()) != 0) _exit(126);
        execv(argv_owned.front().c_str(), argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(in_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[1]);
    result.launched = true;
    if (!stdin_text.empty()) {
        const char* data = stdin_text.data();
        std::size_t remaining = stdin_text.size();
        while (remaining > 0) {
            const auto written = write(in_pipe[1], data, remaining);
            if (written <= 0) {
                break;
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
    }
    close(in_pipe[1]);

    std::string stdout_buf;
    std::string stderr_buf;
    bool callback_aborted = false;
    bool exited = false;
    int status = 0;

    while (!exited) {
        if (cancel && cancel->is_cancelled()) {
            result.cancelled = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = true;
            break;
        }
        if (timeout_ms > 0 && ElapsedMs(started_at) >= timeout_ms) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = true;
            break;
        }
        if (callback_aborted) {
            result.cancelled = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = true;
            break;
        }

        char buf[4096];
        ssize_t n = read(out_pipe[0], buf, sizeof(buf));
        if (n > 0) {
            stdout_buf.append(buf, n);
            DrainLines(stdout_buf, [&](const std::string& line) {
                if (callback_aborted) return;
                if (!on_line(line)) callback_aborted = true;
            });
        }
        ssize_t en = read(err_pipe[0], buf, sizeof(buf));
        if (en > 0 && stderr_buf.size() < 64 * 1024) {
            stderr_buf.append(buf, en);
        }

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            // Drain any remaining output.
            while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0) {
                stdout_buf.append(buf, n);
            }
            DrainLines(stdout_buf, [&](const std::string& line) {
                if (callback_aborted) return;
                if (!on_line(line)) callback_aborted = true;
            });
            if (!stdout_buf.empty() && !callback_aborted) {
                on_line(stdout_buf);
                stdout_buf.clear();
            }
            while ((en = read(err_pipe[0], buf, sizeof(buf))) > 0) {
                if (stderr_buf.size() < 64 * 1024) stderr_buf.append(buf, en);
            }
            exited = true;
            break;
        }
        if (n <= 0 && en <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = status;
    }
    result.stderr_text = std::move(stderr_buf);
#endif

    result.duration_ms = ElapsedMs(started_at);
    if (result.cancelled) {
        result.error_code = "Cancelled";
        result.error_message = "agent invocation was cancelled";
    } else if (result.timed_out) {
        result.error_code = "Timeout";
        result.error_message = "codex CLI timed out";
    } else if (result.exit_code != 0) {
        result.error_code = "ExternalProcessFailed";
        result.error_message = "codex CLI exited with non-zero status";
    }
    return result;
}

// ----- Codex NDJSON event mapping -------------------------------------------
//
// The real Codex CLI emits one JSON object per line on stdout. The shapes vary
// across versions; we tolerate unknown payloads by falling back to a Status
// event so the orchestrator still observes the line. The mapping mirrors
// ductor's parse_codex_jsonl.

void EmitFromCodexLine(
    const std::string& line,
    const AgentEventCallback& on_event,
    bool& abort_flag,
    AgentUsage& usage_acc,
    std::string& session_id_out,
    std::string& assistant_text,
    std::string& final_text,
    std::string& error_code,
    std::string& error_message) {
    if (line.empty()) return;
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(line);
    } catch (...) {
        if (on_event && !abort_flag) {
            AgentEvent ev;
            ev.kind = AgentEvent::Kind::Status;
            ev.payload_text = line;
            if (!on_event(ev)) abort_flag = true;
        }
        return;
    }

    auto get_str = [&](const nlohmann::json& obj, const char* key) -> std::string {
        if (!obj.contains(key)) return {};
        const auto& v = obj.at(key);
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_number_float()) return std::to_string(v.get<double>());
        return {};
    };

    // Codex-style: {"type":"...","msg":{...}} or {"type":"...","data":{...}}
    std::string type = get_str(parsed, "type");
    nlohmann::json payload = nlohmann::json::object();
    if (parsed.contains("msg") && parsed.at("msg").is_object()) {
        payload = parsed.at("msg");
    } else if (parsed.contains("data") && parsed.at("data").is_object()) {
        payload = parsed.at("data");
    } else {
        payload = parsed;
    }

    auto emit = [&](AgentEvent ev) {
        if (!on_event || abort_flag) return;
        if (!on_event(ev)) abort_flag = true;
    };

    if (type == "system" || type == "session.created" || type == "system/init" ||
        type == "session_init" || type == "thread.started") {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::SessionInit;
        const auto sid = get_str(payload, "session_id");
        const auto thread_id = get_str(payload, "thread_id");
        const auto model = get_str(payload, "model");
        if (!sid.empty()) {
            ev.fields["session_id"] = sid;
            session_id_out = sid;
        } else if (!thread_id.empty()) {
            ev.fields["session_id"] = thread_id;
            session_id_out = thread_id;
        }
        if (!model.empty()) ev.fields["model"] = model;
        emit(ev);
    } else if (type == "assistant.message.delta" || type == "message.delta" || type == "text.delta" ||
               type == "agent_message_delta") {
        std::string chunk = get_str(payload, "delta");
        if (chunk.empty()) chunk = get_str(payload, "text");
        if (chunk.empty()) chunk = get_str(payload, "content");
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::TextDelta;
        ev.payload_text = chunk;
        assistant_text += chunk;
        emit(ev);
    } else if (type == "assistant.message" || type == "agent_message") {
        std::string chunk = get_str(payload, "text");
        if (chunk.empty()) chunk = get_str(payload, "content");
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::TextDelta;
        ev.payload_text = chunk;
        assistant_text += chunk;
        emit(ev);
    } else if (type == "thinking" || type == "reasoning" || type == "agent_reasoning_delta") {
        std::string chunk = get_str(payload, "delta");
        if (chunk.empty()) chunk = get_str(payload, "text");
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Thinking;
        ev.payload_text = chunk;
        emit(ev);
    } else if (type == "tool_use" || type == "tool.start" || type == "exec_command_begin") {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::ToolUseStart;
        ev.fields["tool_name"] = get_str(payload, "tool_name");
        if (ev.fields["tool_name"].empty()) ev.fields["tool_name"] = get_str(payload, "name");
        ev.fields["args_json"] = payload.dump();
        emit(ev);
    } else if (type == "tool_result" || type == "tool.end" || type == "exec_command_end") {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::ToolUseResult;
        ev.fields["tool_name"] = get_str(payload, "tool_name");
        ev.fields["success"] = payload.value("success", true) ? "true" : "false";
        ev.fields["output_json"] = payload.dump();
        emit(ev);
    } else if (type == "item.completed") {
        if (parsed.contains("item") && parsed.at("item").is_object()) {
            const auto& item = parsed.at("item");
            const auto item_type = get_str(item, "type");
            if (item_type == "agent_message") {
                const auto text = get_str(item, "text");
                if (!text.empty()) {
                    AgentEvent ev;
                    ev.kind = AgentEvent::Kind::TextDelta;
                    ev.payload_text = text;
                    assistant_text += text;
                    final_text = text;
                    emit(ev);
                }
            }
        }
    } else if (type == "usage" || type == "token_count" || type == "token_usage" ||
               type == "turn.completed") {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Usage;
        const auto& usage_payload =
            (type == "turn.completed" && parsed.contains("usage") && parsed.at("usage").is_object())
                ? parsed.at("usage")
                : payload;
        const auto in_tokens = static_cast<int>(usage_payload.value("input_tokens", 0));
        const auto out_tokens = static_cast<int>(usage_payload.value("output_tokens", 0));
        const auto reasoning_tokens = static_cast<int>(
            usage_payload.value("reasoning_tokens", usage_payload.value("reasoning_output_tokens", 0)));
        const auto cost = usage_payload.value("cost_usd", 0.0);
        ev.fields["input_tokens"] = std::to_string(in_tokens);
        ev.fields["output_tokens"] = std::to_string(out_tokens);
        ev.fields["reasoning_tokens"] = std::to_string(reasoning_tokens);
        ev.fields["cost_usd"] = std::to_string(cost);
        usage_acc.input_tokens += in_tokens;
        usage_acc.output_tokens += out_tokens;
        usage_acc.reasoning_tokens += reasoning_tokens;
        usage_acc.cost_usd += cost;
        emit(ev);
    } else if (type == "task_complete" || type == "final" || type == "result") {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Final;
        std::string text = get_str(payload, "last_agent_message");
        if (text.empty()) text = get_str(payload, "text");
        if (text.empty()) text = get_str(payload, "content");
        if (!text.empty()) {
            ev.payload_text = text;
            final_text = text;
        }
        emit(ev);
    } else if (type == "error") {
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Error;
        ev.fields["error_code"] = get_str(payload, "error_code");
        if (ev.fields["error_code"].empty()) ev.fields["error_code"] = "UpstreamError";
        ev.fields["error_message"] = get_str(payload, "message");
        if (ev.fields["error_message"].empty()) ev.fields["error_message"] = get_str(payload, "error");
        error_code = ev.fields["error_code"];
        error_message = ev.fields["error_message"];
        emit(ev);
    } else {
        // Unknown event — surface verbatim as a status line for observability.
        AgentEvent ev;
        ev.kind = AgentEvent::Kind::Status;
        ev.payload_text = line;
        emit(ev);
    }
}

}  // namespace

CodexCliAgent::CodexCliAgent(const CliHost& cli_host, std::filesystem::path workspace_root)
    : cli_host_(cli_host),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))) {}

AgentProfile CodexCliAgent::profile() const {
    return {
        .agent_name = "codex_cli",
        .version = "0.2.0",
        .description = "Adapter for Codex CLI non-interactive execution.",
        .capabilities = {
            {"code_reasoning", 95},
            {"planning", 85},
            {"patch_generation", 80},
        },
        .supports_session = true,
        .supports_streaming = true,
        .supports_patch = true,
        .supports_subagents = false,
        .supports_network = true,
        .cost_tier = "model-dependent",
        .latency_tier = "medium",
        .risk_level = "medium",
    };
}

bool CodexCliAgent::healthy() const {
    return CommandExists("codex") || CommandExists("codex.cmd");
}

std::string CodexCliAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "codex-cli-session-" + std::to_string(next_id);
}

void CodexCliAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult CodexCliAgent::run_task(const AgentTask& task) {
    if (!healthy()) {
        return {
            .success = false,
            .error_code = "AgentUnavailable",
            .error_message = "codex CLI was not found on PATH",
        };
    }

    const auto requested_workspace = task.workspace_path.empty()
        ? workspace_root_
        : std::filesystem::path(task.workspace_path);
    const auto workspace_path = NormalizeWorkspaceRoot(requested_workspace);
    std::error_code workspace_ec;
    if (!std::filesystem::exists(workspace_path, workspace_ec) ||
        !std::filesystem::is_directory(workspace_path, workspace_ec)) {
        return {
            .success = false,
            .error_code = "InvalidWorkspace",
            .error_message = "agent workspace must be an existing directory",
        };
    }

    const auto output_dir = workspace_path / "runtime" / "agents" / "codex_cli" / SafeFileStem(task.task_id);
    std::filesystem::create_directories(output_dir);
    const auto output_file = output_dir / "last_message.txt";
    const bool allow_writes = AllowsWorkspaceWrites(task);

    CliSpec spec{
        .name = "codex_cli_agent",
        .description = "Run Codex CLI in non-interactive mode.",
        .binary = "codex",
        .args_template = {
            "exec",
            "--sandbox",
            allow_writes ? "workspace-write" : "read-only",
            "--skip-git-repo-check",
            "--color",
            "never",
        },
        .required_args = {"workspace_path", "output_file", "prompt"},
        .input_schema_json = R"({"type":"object","required":["prompt"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = allow_writes
            ? std::vector<std::string>{"filesystem.read", "filesystem.write", "process.spawn", "network.access"}
            : std::vector<std::string>{"filesystem.read", "process.spawn", "network.access"},
        .timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : 120000,
        .output_limit_bytes = 1024 * 1024,
        .env_allowlist = {"USERPROFILE", "HOMEDRIVE", "HOMEPATH", "HOME", "APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME", "CODEX_HOME"},
    };

    if (task.auth_profile.has_value() && !task.auth_profile->empty()) {
        spec.args_template.push_back("--profile");
        spec.args_template.push_back(*task.auth_profile);
    }
    if (allow_writes) {
        spec.args_template.push_back("--full-auto");
    }
    spec.args_template.insert(spec.args_template.end(), {
        "--output-last-message",
        "{{output_file}}",
        "-C",
        "{{workspace_path}}",
        "{{prompt}}",
    });

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"workspace_path", workspace_path.string()},
            {"output_file", output_file.string()},
            {"prompt", BuildPrompt(task)},
        },
        .workspace_path = workspace_path,
    });

    auto final_message = ReadTextFile(output_file);
    if (final_message.empty()) {
        final_message = result.stdout_text;
    }

    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "codex_cli";
    legacy_output_json["content"] = result.success ? final_message : "";
    legacy_output_json["command"] = result.command_display;
    legacy_output_json["exit_code"] = result.exit_code;
    legacy_output_json["stdout"] = result.stdout_text;
    legacy_output_json["stderr"] = result.stderr_text;
    legacy_output_json["last_message_file"] = output_file.string();
    const auto legacy_output = legacy_output_json.dump();

    nlohmann::ordered_json metadata_json;
    metadata_json["source"] = "codex_cli";
    AgentResult agent_result{
        .success = result.success,
        .summary = result.success ? final_message : "Codex CLI task failed.",
        .structured_output_json = legacy_output,
        .artifacts = {
            AgentArtifact{
                .type = "text",
                .uri = output_file.string(),
                .content = final_message,
                .metadata_json = metadata_json.dump(),
            },
        },
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "codex_cli",
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

AgentResult CodexCliAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool CodexCliAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

AgentResult CodexCliAgent::invoke(const AgentInvocation& invocation, const AgentEventCallback& on_event) {
    if (!healthy()) {
        AgentResult r;
        r.success = false;
        r.error_code = "AgentUnavailable";
        r.error_message = "codex CLI was not found on PATH";
        return r;
    }

    auto requested_workspace = invocation.workspace_path.empty() ? workspace_root_ : invocation.workspace_path;
    auto workspace_path = NormalizeWorkspaceRoot(requested_workspace);
    std::error_code workspace_ec;
    if (!std::filesystem::exists(workspace_path, workspace_ec) ||
        !std::filesystem::is_directory(workspace_path, workspace_ec)) {
        AgentResult r;
        r.success = false;
        r.error_code = "InvalidWorkspace";
        r.error_message = "agent workspace must be an existing directory";
        return r;
    }
    const auto output_dir = workspace_path / "runtime" / "agents" / "codex_cli" / SafeFileStem(invocation.task_id);
    std::filesystem::create_directories(output_dir);
    const auto output_file = output_dir / "last_message.txt";
    const auto prompt_file = output_dir / "prompt.txt";

    // Argument layout. We assume the real Codex CLI supports `--json` (or
    // equivalent NDJSON) on `exec`, and `exec resume -- <id> <prompt>` for
    // resume. Both are documented in the audit notes; the test fixture below
    // exercises the same argv shape so we get coverage without the real CLI.
    std::vector<std::string> args;
    const auto prompt = BuildPromptV2(invocation);
    WriteTextFile(prompt_file, prompt);
    const bool allow_writes = AllowsWorkspaceWrites(invocation);
    if (invocation.resume_session_id.has_value() && !invocation.resume_session_id->empty()) {
        args = {
            "exec",
            "resume",
            "--json",
            "--skip-git-repo-check",
            "--output-last-message", output_file.string(),
        };
        if (allow_writes) {
            args.push_back("--full-auto");
        }
        args.push_back("--");
        args.push_back(*invocation.resume_session_id);
        args.push_back("-");
    } else {
        args = {
            "exec",
            "--json",
            "--sandbox", allow_writes ? "workspace-write" : "read-only",
            "--skip-git-repo-check",
            "--color", "never",
            "--output-last-message", output_file.string(),
            "-C", workspace_path.string(),
        };
        if (invocation.auth_profile.has_value() && !invocation.auth_profile->empty()) {
            args.push_back("--profile");
            args.push_back(*invocation.auth_profile);
        }
        if (allow_writes) {
            args.push_back("--full-auto");
        }
        args.push_back("-");
    }

    bool abort_flag = false;
    AgentUsage usage_acc;
    std::string session_id_seen;
    std::string assistant_text;
    std::string final_text;
    std::string upstream_error_code;
    std::string upstream_error_message;
    int event_count = 0;

    auto on_line = [&](const std::string& line) -> bool {
        if (line.empty()) return true;
        ++event_count;
        EmitFromCodexLine(
            line, on_event, abort_flag,
            usage_acc, session_id_seen,
            assistant_text, final_text,
            upstream_error_code, upstream_error_message);
        return !abort_flag;
    };

    const auto stream_result = RunStreamingProcess(
        "codex",
        args,
        prompt,
        workspace_path,
        invocation.timeout_ms > 0 ? invocation.timeout_ms : 120000,
        invocation.cancel,
        on_line);

    AgentResult agent_result;
    agent_result.duration_ms = stream_result.duration_ms;
    agent_result.usage = usage_acc;
    if (!session_id_seen.empty()) {
        agent_result.session_id = session_id_seen;
    }

    auto effective_text = !final_text.empty() ? final_text : assistant_text;
    if (effective_text.empty()) {
        effective_text = ReadTextFile(output_file);
    }

    if (stream_result.cancelled) {
        agent_result.success = false;
        agent_result.error_code = "Cancelled";
        agent_result.error_message = "agent invocation was cancelled";
    } else if (stream_result.timed_out) {
        agent_result.success = false;
        agent_result.error_code = "Timeout";
        agent_result.error_message = "codex CLI timed out";
    } else if (!stream_result.launched) {
        agent_result.success = false;
        agent_result.error_code = stream_result.error_code.empty() ? "AgentUnavailable" : stream_result.error_code;
        agent_result.error_message = stream_result.error_message;
    } else if (!upstream_error_code.empty()) {
        agent_result.success = false;
        agent_result.error_code = upstream_error_code;
        agent_result.error_message = upstream_error_message;
    } else if (stream_result.exit_code != 0) {
        agent_result.success = false;
        agent_result.error_code = stream_result.error_code.empty() ? "ExternalProcessFailed" : stream_result.error_code;
        agent_result.error_message = stream_result.error_message.empty()
            ? "codex CLI exited with non-zero status"
            : stream_result.error_message;
    } else {
        agent_result.success = true;
    }

    agent_result.summary = effective_text.empty()
        ? (agent_result.success ? std::string("(no codex output)") : std::string("Codex CLI task failed."))
        : effective_text;

    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "codex_cli";
    legacy_output_json["content"] = agent_result.success ? effective_text : "";
    legacy_output_json["command"] = stream_result.command_display;
    legacy_output_json["exit_code"] = stream_result.exit_code;
    legacy_output_json["event_count"] = event_count;
    legacy_output_json["last_message_file"] = output_file.string();
    legacy_output_json["prompt_file"] = prompt_file.string();
    legacy_output_json["session_id"] = session_id_seen;
    legacy_output_json["input_tokens"] = usage_acc.input_tokens;
    legacy_output_json["output_tokens"] = usage_acc.output_tokens;
    legacy_output_json["reasoning_tokens"] = usage_acc.reasoning_tokens;
    legacy_output_json["cost_usd"] = usage_acc.cost_usd;
    legacy_output_json["stderr"] = stream_result.stderr_text;
    const auto legacy_output = legacy_output_json.dump();
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "codex_cli",
        .success = agent_result.success,
        .summary = agent_result.summary,
        .structured_output_json = legacy_output,
        .artifacts = agent_result.artifacts,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = usage_acc.cost_usd,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    });
    return agent_result;
}

std::string CodexCliAgent::BuildPrompt(const AgentTask& task) {
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
            << "- If the user did not specify a path, choose a reasonable project/example path in this workspace.\n"
            << "- At the end, list changed files and how to build or run the result.\n\n";
        if (!task.context_json.empty()) {
            try {
                const auto context = nlohmann::json::parse(task.context_json);
                if (context.is_object() && context.contains("contract_instructions") &&
                    context["contract_instructions"].is_string()) {
                    prompt << "ACCEPTANCE CONTRACT:\n"
                           << context["contract_instructions"].get<std::string>() << "\n";
                    if (context.contains("deliverables_manifest") &&
                        context["deliverables_manifest"].is_string()) {
                        prompt << "Write the deliverables manifest exactly here: "
                               << context["deliverables_manifest"].get<std::string>() << "\n"
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
            } catch (const nlohmann::json::exception&) {
            }
        }
    } else {
        prompt
            << "You are running as a secondary expert agent inside AgentOS.\n"
            << "Operate in read-only mode unless the invocation explicitly enables workspace writes.\n"
            << "Return a concise structured summary with findings, suggested next steps, and risks.\n\n"
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

std::string CodexCliAgent::BuildPromptV2(const AgentInvocation& invocation) {
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
            << "- If the user did not specify a path, choose a reasonable project/example path in this workspace.\n"
            << "- At the end, list changed files and how to build or run the result.\n\n";
        const auto contract_it = invocation.context.find("contract_instructions");
        if (contract_it != invocation.context.end() && !contract_it->second.empty()) {
            prompt << "ACCEPTANCE CONTRACT:\n" << contract_it->second << "\n";
            const auto manifest_it = invocation.context.find("deliverables_manifest");
            if (manifest_it != invocation.context.end() && !manifest_it->second.empty()) {
                prompt << "Write the deliverables manifest exactly here: " << manifest_it->second << "\n"
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
            << "You are running as a secondary expert agent inside AgentOS.\n"
            << "Operate in read-only mode unless the invocation explicitly enables workspace writes.\n"
            << "Return a concise structured summary with findings, suggested next steps, and risks.\n\n"
            << "Objective: " << invocation.objective << "\n";
    }
    prompt
        << "Task id: " << invocation.task_id << "\n"
        << "Workspace: " << invocation.workspace_path.string() << "\n";

    if (!invocation.context.empty()) {
        nlohmann::json ctx = nlohmann::json::object();
        for (const auto& [k, v] : invocation.context) ctx[k] = v;
        prompt << "Context JSON: " << ctx.dump() << "\n";
    }
    if (!invocation.constraints.empty()) {
        nlohmann::json cons = nlohmann::json::object();
        for (const auto& [k, v] : invocation.constraints) cons[k] = v;
        prompt << "Constraints JSON: " << cons.dump() << "\n";
    }
    if (!invocation.attachments.empty()) {
        prompt << "Attachments:\n";
        for (const auto& a : invocation.attachments) prompt << "  - " << a << "\n";
    }
    return prompt.str();
}

std::string CodexCliAgent::SafeFileStem(std::string value) {
    std::replace_if(value.begin(), value.end(), [](const unsigned char ch) {
        return !std::isalnum(ch) && ch != '-' && ch != '_';
    }, '_');

    if (value.empty()) {
        return "task";
    }
    return value;
}

}  // namespace agentos
