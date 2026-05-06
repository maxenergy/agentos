#pragma once

#include "hosts/plugin/plugin_host.hpp"

#include "hosts/plugin/plugin_json_rpc.hpp"
#include "utils/command_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
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

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

}  // namespace

inline std::string RenderPersistentTemplateValue(std::string value, const StringMap& arguments, const std::filesystem::path& workspace_path) {
    StringMap values = arguments;
    values["cwd"] = workspace_path.string();
    for (const auto& [key, replacement] : values) {
        const auto placeholder = "{{" + key + "}}";
        std::size_t position = 0;
        while ((position = value.find(placeholder, position)) != std::string::npos) {
            value.replace(position, placeholder.size(), replacement);
            position += replacement.size();
        }
    }
    return value;
}

inline std::vector<std::string> RenderPersistentArgs(
    const PluginSpec& spec,
    const StringMap& arguments,
    const std::filesystem::path& workspace_path) {
    std::vector<std::string> args;
    args.reserve(spec.args_template.size());
    for (const auto& value : spec.args_template) {
        args.push_back(RenderPersistentTemplateValue(value, arguments, workspace_path));
    }
    return args;
}

struct PersistentPluginSession {
    PluginSpec spec;
    std::filesystem::path workspace_path;
    int next_request_id = 1;
    int request_count = 0;
    std::string stderr_text;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point started_at_wall = std::chrono::system_clock::now();
    std::chrono::steady_clock::time_point last_used_at = std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point last_used_at_wall = std::chrono::system_clock::now();

#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
#else
    pid_t child_pid = -1;
    int stdin_write = -1;
    int stdout_read = -1;
    int stderr_read = -1;
#endif

    ~PersistentPluginSession() {
        close();
    }

    void close() {
#ifdef _WIN32
        if (process != nullptr) {
            TerminateProcess(process, 125);
            WaitForSingleObject(process, 1000);
        }
        if (stdin_write != nullptr) {
            CloseHandle(stdin_write);
            stdin_write = nullptr;
        }
        if (stdout_read != nullptr) {
            CloseHandle(stdout_read);
            stdout_read = nullptr;
        }
        if (stderr_read != nullptr) {
            CloseHandle(stderr_read);
            stderr_read = nullptr;
        }
        if (thread != nullptr) {
            CloseHandle(thread);
            thread = nullptr;
        }
        if (process != nullptr) {
            CloseHandle(process);
            process = nullptr;
        }
#else
        if (child_pid > 0) {
            kill(-child_pid, SIGTERM);
            int status = 0;
            for (int index = 0; index < 20; ++index) {
                if (waitpid(child_pid, &status, WNOHANG) == child_pid) {
                    child_pid = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (child_pid > 0) {
                kill(-child_pid, SIGKILL);
                (void)waitpid(child_pid, &status, 0);
                child_pid = -1;
            }
        }
        if (stdin_write != -1) {
            close_fd(stdin_write);
            stdin_write = -1;
        }
        if (stdout_read != -1) {
            close_fd(stdout_read);
            stdout_read = -1;
        }
        if (stderr_read != -1) {
            close_fd(stderr_read);
            stderr_read = -1;
        }
#endif
    }

#ifndef _WIN32
    static void close_fd(const int fd) {
        if (fd != -1) {
            ::close(fd);
        }
    }

    static bool set_nonblocking(const int fd) {
        const int flags = fcntl(fd, F_GETFL, 0);
        return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
    }
#endif

    static std::unique_ptr<PersistentPluginSession> start(
        const PluginSpec& spec,
        const std::filesystem::path& workspace_path,
        std::string& error_message) {
        auto session = std::make_unique<PersistentPluginSession>();
        session->spec = spec;
        session->workspace_path = workspace_path;
        session->started_at = std::chrono::steady_clock::now();
        session->started_at_wall = std::chrono::system_clock::now();
        session->last_used_at = session->started_at;
        session->last_used_at_wall = session->started_at_wall;
        const auto args = RenderPersistentArgs(spec, {}, workspace_path);

#ifdef _WIN32
        SECURITY_ATTRIBUTES security_attributes{};
        security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        security_attributes.bInheritHandle = TRUE;

        HANDLE stdin_read = nullptr;
        HANDLE stdout_write = nullptr;
        HANDLE stderr_write = nullptr;
        if (!CreatePipe(&stdin_read, &session->stdin_write, &security_attributes, 0) ||
            !CreatePipe(&session->stdout_read, &stdout_write, &security_attributes, 0) ||
            !CreatePipe(&session->stderr_read, &stderr_write, &security_attributes, 0)) {
            error_message = "failed to create persistent plugin pipes";
            session->close();
            return nullptr;
        }
        SetHandleInformation(session->stdin_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(session->stdout_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(session->stderr_read, HANDLE_FLAG_INHERIT, 0);

        // Same handle-inheritance hardening as CliHost: bInheritHandles=TRUE
        // would otherwise inherit every inheritable parent-side handle into
        // the long-running plugin child, including CTest's stdout/stderr
        // pipes when the test binary that owns this PluginHost runs under
        // CTest. STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_HANDLE_LIST narrows
        // the inheritance to exactly stdin_read / stdout_write / stderr_write.
        HANDLE inherit_list[3] = {stdin_read, stdout_write, stderr_write};
        SIZE_T inherit_count = 3;

        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<unsigned char> attribute_buffer(attribute_size);
        auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
        bool attribute_list_initialized = false;
        if (InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size)) {
            attribute_list_initialized = true;
            if (!UpdateProcThreadAttribute(
                    attribute_list,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inherit_list,
                    inherit_count * sizeof(HANDLE),
                    nullptr,
                    nullptr)) {
                DeleteProcThreadAttributeList(attribute_list);
                attribute_list_initialized = false;
            }
        }

        STARTUPINFOEXA startup_info{};
        startup_info.StartupInfo.cb = sizeof(STARTUPINFOEXA);
        startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup_info.StartupInfo.hStdInput = stdin_read;
        startup_info.StartupInfo.hStdOutput = stdout_write;
        startup_info.StartupInfo.hStdError = stderr_write;
        if (attribute_list_initialized) {
            startup_info.lpAttributeList = attribute_list;
        }

        PROCESS_INFORMATION process_info{};
        auto command_line = QuoteCommandForDisplay(spec.binary);
        for (const auto& arg : args) {
            command_line += " " + QuoteCommandForDisplay(arg);
        }
        auto mutable_command_line = command_line;
        auto cwd_string = workspace_path.string();
        DWORD creation_flags = CREATE_NO_WINDOW;
        if (attribute_list_initialized) {
            creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
        }
        const BOOL created = CreateProcessA(
            nullptr,
            mutable_command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            creation_flags,
            nullptr,
            cwd_string.c_str(),
            reinterpret_cast<LPSTARTUPINFOA>(&startup_info),
            &process_info);
        if (attribute_list_initialized) {
            DeleteProcThreadAttributeList(attribute_list);
        }
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);
        if (!created) {
            error_message = "failed to start persistent plugin process";
            session->close();
            return nullptr;
        }
        session->process = process_info.hProcess;
        session->thread = process_info.hThread;
#else
        int stdin_pipe[2] = {-1, -1};
        int stdout_pipe[2] = {-1, -1};
        int stderr_pipe[2] = {-1, -1};
        if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
            error_message = "failed to create persistent plugin pipes";
            close_fd(stdin_pipe[0]);
            close_fd(stdin_pipe[1]);
            close_fd(stdout_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stderr_pipe[0]);
            close_fd(stderr_pipe[1]);
            return nullptr;
        }

        const pid_t child = fork();
        if (child == -1) {
            error_message = std::string("failed to fork persistent plugin process: ") + std::strerror(errno);
            close_fd(stdin_pipe[0]);
            close_fd(stdin_pipe[1]);
            close_fd(stdout_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stderr_pipe[0]);
            close_fd(stderr_pipe[1]);
            return nullptr;
        }
        if (child == 0) {
            (void)setpgid(0, 0);
            close_fd(stdin_pipe[1]);
            close_fd(stdout_pipe[0]);
            close_fd(stderr_pipe[0]);
            (void)dup2(stdin_pipe[0], STDIN_FILENO);
            (void)dup2(stdout_pipe[1], STDOUT_FILENO);
            (void)dup2(stderr_pipe[1], STDERR_FILENO);
            close_fd(stdin_pipe[0]);
            close_fd(stdout_pipe[1]);
            close_fd(stderr_pipe[1]);
            (void)chdir(workspace_path.string().c_str());
            std::vector<std::string> arg_values = args;
            arg_values.insert(arg_values.begin(), spec.binary);
            std::vector<char*> argv;
            for (auto& value : arg_values) {
                argv.push_back(value.data());
            }
            argv.push_back(nullptr);
            execvp(spec.binary.c_str(), argv.data());
            _exit(127);
        }

        (void)setpgid(child, child);
        close_fd(stdin_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[1]);
        session->child_pid = child;
        session->stdin_write = stdin_pipe[1];
        session->stdout_read = stdout_pipe[0];
        session->stderr_read = stderr_pipe[0];
        (void)set_nonblocking(session->stdout_read);
        (void)set_nonblocking(session->stderr_read);
#endif
        return session;
    }

    bool alive() const {
#ifdef _WIN32
        if (process == nullptr) {
            return false;
        }
        DWORD exit_code = 0;
        return GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
#else
        if (child_pid <= 0) {
            return false;
        }
        int status = 0;
        const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
        return wait_result == 0;
#endif
    }

    std::int64_t pid_value() const {
#ifdef _WIN32
        return process == nullptr ? 0 : static_cast<std::int64_t>(GetProcessId(process));
#else
        return child_pid > 0 ? static_cast<std::int64_t>(child_pid) : 0;
#endif
    }

    bool idle_expired(const int idle_timeout_ms) const {
        return ElapsedMs(last_used_at) >= idle_timeout_ms;
    }

    // Polls until the plugin process has been alive continuously for `kReadyDwellMs`
    // (a readiness dwell), or `timeout_ms` elapses, or the process exits.
    // The dwell catches plugins that fork/exec then exit immediately — `alive()`
    // alone returning true after 5 ms is insufficient evidence of readiness.
    bool wait_until_started(const int timeout_ms) const {
        constexpr int kReadyDwellMs = 25;
        constexpr int kPollIntervalMs = 5;
        const auto wait_started_at = std::chrono::steady_clock::now();
        auto first_alive_at = std::chrono::steady_clock::time_point{};
        bool seen_alive = false;
        while (ElapsedMs(wait_started_at) < timeout_ms) {
            if (!alive()) {
                if (seen_alive) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
                continue;
            }
            if (!seen_alive) {
                seen_alive = true;
                first_alive_at = std::chrono::steady_clock::now();
            }
            if (ElapsedMs(first_alive_at) >= kReadyDwellMs) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
        }
        return seen_alive && alive();
    }

    void drain_stderr() {
        char buffer[512];
#ifdef _WIN32
        DWORD available = 0;
        while (stderr_read != nullptr && PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD bytes_read = 0;
            const DWORD to_read = std::min<DWORD>(available, sizeof(buffer));
            if (!ReadFile(stderr_read, buffer, to_read, &bytes_read, nullptr) || bytes_read == 0) {
                break;
            }
            stderr_text.append(buffer, bytes_read);
        }
#else
        while (stderr_read != -1) {
            const ssize_t bytes_read = read(stderr_read, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                stderr_text.append(buffer, static_cast<std::size_t>(bytes_read));
                continue;
            }
            break;
        }
#endif
        if (stderr_text.size() > spec.output_limit_bytes) {
            stderr_text = stderr_text.substr(stderr_text.size() - spec.output_limit_bytes);
        }
    }

    bool write_line(const std::string& line) {
        const auto payload = line + "\n";
#ifdef _WIN32
        DWORD bytes_written = 0;
        return stdin_write != nullptr &&
            WriteFile(stdin_write, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr) &&
            bytes_written == payload.size();
#else
        const char* data = payload.data();
        std::size_t remaining = payload.size();
        while (remaining > 0) {
            const ssize_t written = write(stdin_write, data, remaining);
            if (written <= 0) {
                return false;
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return true;
#endif
    }

    std::optional<std::string> read_response_line(const int timeout_ms, bool& timed_out) {
        std::string line;
        const auto read_started_at = std::chrono::steady_clock::now();
        while (true) {
            drain_stderr();
            if (ElapsedMs(read_started_at) >= timeout_ms) {
                timed_out = true;
                return std::nullopt;
            }

            char ch = '\0';
#ifdef _WIN32
            DWORD available = 0;
            if (stdout_read != nullptr && PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD bytes_read = 0;
                if (ReadFile(stdout_read, &ch, 1, &bytes_read, nullptr) && bytes_read == 1) {
                    if (ch == '\n') {
                        return line;
                    }
                    if (ch != '\r') {
                        line.push_back(ch);
                    }
                    if (line.size() > spec.output_limit_bytes) {
                        timed_out = false;
                        return std::nullopt;
                    }
                    continue;
                }
            }
#else
            const ssize_t bytes_read = read(stdout_read, &ch, 1);
            if (bytes_read == 1) {
                if (ch == '\n') {
                    return line;
                }
                if (ch != '\r') {
                    line.push_back(ch);
                }
                if (line.size() > spec.output_limit_bytes) {
                    timed_out = false;
                    return std::nullopt;
                }
                continue;
            }
#endif
            if (!alive()) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    PluginRunResult request(const StringMap& arguments, const bool health_probe = false) {
        const auto request_started_at = std::chrono::steady_clock::now();
        if (!alive()) {
            return {
                .success = false,
                .duration_ms = ElapsedMs(request_started_at),
                .error_code = "PluginLifecycleUnavailable",
                .error_message = "persistent plugin process is not running",
            };
        }

        const auto request_json = health_probe
            ? JsonRpcHealthRequest(next_request_id++)
            : JsonRpcRequestForPlugin(spec, arguments, next_request_id++);
        ++request_count;
        last_used_at = std::chrono::steady_clock::now();
        last_used_at_wall = std::chrono::system_clock::now();
        if (!write_line(request_json)) {
            close();
            return {
                .success = false,
                .duration_ms = ElapsedMs(request_started_at),
                .error_code = "PluginLifecycleWriteFailed",
                .error_message = "failed to write JSON-RPC request to persistent plugin",
            };
        }

        bool timed_out = false;
        const auto line = read_response_line(spec.timeout_ms, timed_out);
        if (!line.has_value()) {
            close();
            return {
                .success = false,
                .timed_out = timed_out,
                .duration_ms = ElapsedMs(request_started_at),
                .stderr_text = stderr_text,
                .error_code = timed_out ? "Timeout" : "PluginLifecycleReadFailed",
                .error_message = timed_out
                    ? "persistent plugin JSON-RPC request timed out"
                    : "failed to read JSON-RPC response from persistent plugin",
            };
        }

        return {
            .success = true,
            .exit_code = 0,
            .duration_ms = ElapsedMs(started_at),
            .stdout_text = *line,
            .stderr_text = stderr_text,
        };
    }
};


}  // namespace agentos
