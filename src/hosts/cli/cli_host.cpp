#include "hosts/cli/cli_host.hpp"

#include "utils/command_utils.hpp"
#include "utils/path_utils.hpp"
#include "utils/secret_redaction.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
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

bool ContainsWhitespaceOrQuote(const std::string& value) {
    return value.empty() || std::any_of(value.begin(), value.end(), [](const unsigned char ch) {
        return std::isspace(ch) != 0 || ch == '"' || ch == '&' || ch == '<' || ch == '>' || ch == '|' || ch == '^';
    });
}

std::string BuildCommandLine(const std::string& binary, const std::vector<std::string>& args) {
    std::ostringstream command;
    command << QuoteCommandForDisplay(binary);

    for (const auto& arg : args) {
        command << ' ' << QuoteCommandForDisplay(arg);
    }

    return command.str();
}

CliRunResult RedactCliRunResult(CliRunResult result, const StringMap& arguments) {
    result.command_display = RedactSensitiveText(std::move(result.command_display), arguments);
    result.stdout_text = RedactSensitiveText(std::move(result.stdout_text), arguments);
    result.stderr_text = RedactSensitiveText(std::move(result.stderr_text), arguments);
    result.error_message = RedactSensitiveText(std::move(result.error_message), arguments);
    return result;
}

std::string ClipOutput(const std::string& value, const std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }

    return value.substr(0, limit) + "\n[agentos: output truncated]";
}

std::string ReadFileClipped(const std::filesystem::path& path, const std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string output;
    std::array<char, 4096> buffer{};
    while (input && output.size() < limit) {
        input.read(buffer.data(), static_cast<std::streamsize>(
            std::min<std::size_t>(buffer.size(), limit - output.size())));
        const auto count = input.gcount();
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    return ClipOutput(output, limit);
}

std::string ToUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

std::optional<std::string> ReadEnvironmentVariable(const std::string& name) {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, name.c_str()) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }

    std::string value(raw_value, value_size > 0 ? value_size - 1 : 0);
    std::free(raw_value);
    return value;
#else
    const char* raw_value = std::getenv(name.c_str());
    if (!raw_value) {
        return std::nullopt;
    }
    return std::string(raw_value);
#endif
}

std::vector<std::string> DefaultEnvironmentAllowlist() {
#ifdef _WIN32
    return {"SystemRoot", "WINDIR", "ComSpec", "PATH", "PATHEXT", "TEMP", "TMP"};
#else
    return {"PATH", "HOME", "TMPDIR", "LANG", "LC_ALL"};
#endif
}

std::vector<std::string> EffectiveEnvironmentAllowlist(const CliSpec& spec) {
    auto names = DefaultEnvironmentAllowlist();
    names.insert(names.end(), spec.env_allowlist.begin(), spec.env_allowlist.end());
    return names;
}

#ifdef _WIN32

std::wstring StringToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(
            code_page, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0) {
        return {};
    }

    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        code_page, flags, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

bool IsBatchFile(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension == ".cmd" || extension == ".bat";
}

std::string BuildWindowsProcessCommandLine(const std::string& binary, const std::vector<std::string>& args) {
    const auto resolved_binary = ResolveCommandPath(binary).value_or(std::filesystem::path(binary));
    const auto target_command = BuildCommandLine(resolved_binary.string(), args);

    if (IsBatchFile(resolved_binary)) {
        return "cmd.exe /d /s /c \"" + target_command + "\"";
    }

    return target_command;
}

std::vector<wchar_t> BuildWindowsEnvironmentBlock(const CliSpec& spec) {
    std::map<std::string, std::pair<std::string, std::string>> values;

    for (const auto& name : EffectiveEnvironmentAllowlist(spec)) {
        if (name.empty()) {
            continue;
        }

        if (const auto value = ReadEnvironmentVariable(name); value.has_value()) {
            values[ToUpper(name)] = {name, *value};
        }
    }

    std::vector<wchar_t> block;
    for (const auto& [unused_key, entry] : values) {
        (void)unused_key;
        const auto line = entry.first + "=" + entry.second;
        const auto wide_line = StringToWide(line);
        block.insert(block.end(), wide_line.begin(), wide_line.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void ReadPipeToString(HANDLE read_handle, std::string& output, const std::size_t limit) {
    constexpr DWORD kBufferSize = 4096;
    char buffer[kBufferSize];
    DWORD bytes_read = 0;

    while (ReadFile(read_handle, buffer, kBufferSize, &bytes_read, nullptr) && bytes_read > 0) {
        if (output.size() < limit) {
            const auto remaining = limit - output.size();
            output.append(buffer, std::min<std::size_t>(bytes_read, remaining));
        }
    }
}

void CloseIfOpen(HANDLE handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
}

bool CliSpecUsesWindowsJobLimits(const CliSpec& spec) {
    return spec.memory_limit_bytes > 0 || spec.max_processes > 0 || spec.cpu_time_limit_seconds > 0;
}

bool ConfigureJobLimits(const CliSpec& spec, HANDLE job_handle) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (spec.memory_limit_bytes > 0) {
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(spec.memory_limit_bytes);
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    }

    if (spec.max_processes > 0) {
        limits.BasicLimitInformation.ActiveProcessLimit = static_cast<DWORD>(spec.max_processes);
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    }

    if (spec.cpu_time_limit_seconds > 0) {
        limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
            static_cast<LONGLONG>(spec.cpu_time_limit_seconds) * 10'000'000LL;
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_TIME;
    }

    return SetInformationJobObject(
               job_handle,
               JobObjectExtendedLimitInformation,
               &limits,
               static_cast<DWORD>(sizeof(limits))) != FALSE;
}

CliRunResult RunProcessWindows(
    const CliRunRequest& request,
    const std::filesystem::path& cwd,
    const std::vector<std::string>& args) {
    const auto started_at = std::chrono::steady_clock::now();

    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    HANDLE stdin_read = nullptr;
    const auto temp_stem = std::string("agentos-cli-") +
        std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64());
    const auto stdout_path = std::filesystem::temp_directory_path() / (temp_stem + "-stdout.txt");
    const auto stderr_path = std::filesystem::temp_directory_path() / (temp_stem + "-stderr.txt");
    const auto stdout_path_w = StringToWide(stdout_path.string());
    const auto stderr_path_w = StringToWide(stderr_path.string());

    stdout_write = CreateFileW(
        stdout_path_w.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security_attributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    stderr_write = CreateFileW(
        stderr_path_w.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &security_attributes,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (stdout_write == INVALID_HANDLE_VALUE || stderr_write == INVALID_HANDLE_VALUE) {
        CloseIfOpen(stdout_write);
        CloseIfOpen(stderr_write);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "PipeCreateFailed",
            .error_message = "failed to create process output capture files",
        };
    }
    stdin_read = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security_attributes,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (stdin_read == INVALID_HANDLE_VALUE) {
        stdin_read = nullptr;
    }

    // Build STARTUPINFOEX with PROC_THREAD_ATTRIBUTE_HANDLE_LIST so the child
    // inherits exactly stdout_write / stderr_write / stdin_read and nothing
    // else. With plain STARTUPINFOA + bInheritHandles=TRUE the child would
    // inherit every inheritable parent-side handle, including the ctest-
    // owned stdout/stderr pipes when this code runs inside the test binary
    // — which produces an intermittent segfault during pipe teardown when
    // a long-running grandchild (e.g. powershell) outlives the child.
    HANDLE inherit_list[3];
    SIZE_T inherit_count = 0;
    inherit_list[inherit_count++] = stdout_write;
    inherit_list[inherit_count++] = stderr_write;
    if (stdin_read != nullptr && stdin_read != INVALID_HANDLE_VALUE) {
        inherit_list[inherit_count++] = stdin_read;
    }

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

    STARTUPINFOEXW startup_info{};
    startup_info.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_info.StartupInfo.hStdOutput = stdout_write;
    startup_info.StartupInfo.hStdError = stderr_write;
    startup_info.StartupInfo.hStdInput = stdin_read != INVALID_HANDLE_VALUE ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);
    if (attribute_list_initialized) {
        startup_info.lpAttributeList = attribute_list;
    }

    PROCESS_INFORMATION process_info{};
    auto command_display = BuildCommandLine(request.spec.binary, args);
    auto process_command_line = BuildWindowsProcessCommandLine(request.spec.binary, args);
    auto mutable_command_line = StringToWide(process_command_line);
    auto cwd_string = StringToWide(cwd.string());
    auto environment_block = BuildWindowsEnvironmentBlock(request.spec);
    DWORD creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    if (attribute_list_initialized) {
        creation_flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        creation_flags,
        environment_block.data(),
        cwd_string.c_str(),
        reinterpret_cast<LPSTARTUPINFOW>(&startup_info),
        &process_info);

    if (attribute_list_initialized) {
        DeleteProcThreadAttributeList(attribute_list);
    }

    CloseIfOpen(stdout_write);
    stdout_write = nullptr;
    CloseIfOpen(stderr_write);
    stderr_write = nullptr;

    if (!created) {
        CloseIfOpen(stdout_read);
        CloseIfOpen(stderr_read);
        CloseIfOpen(stdin_read);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "ProcessStartFailed",
            .error_message = "failed to start process",
        };
    }

    CloseIfOpen(stdin_read);
    HANDLE job_handle = CreateJobObjectA(nullptr, nullptr);
    if (job_handle == nullptr || !ConfigureJobLimits(request.spec, job_handle) ||
        !AssignProcessToJobObject(job_handle, process_info.hProcess)) {
        TerminateProcess(process_info.hProcess, 125);
        WaitForSingleObject(process_info.hProcess, INFINITE);
        CloseIfOpen(job_handle);
        CloseIfOpen(process_info.hThread);
        CloseIfOpen(process_info.hProcess);
        CloseIfOpen(stdout_read);
        CloseIfOpen(stderr_read);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "ResourceLimitSetupFailed",
            .error_message = "failed to apply Windows job object controls",
        };
    }

    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process_info.hProcess, 125);
        WaitForSingleObject(process_info.hProcess, INFINITE);
        CloseIfOpen(job_handle);
        CloseIfOpen(process_info.hThread);
        CloseIfOpen(process_info.hProcess);
        CloseIfOpen(stdout_read);
        CloseIfOpen(stderr_read);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "ProcessStartFailed",
            .error_message = "failed to resume process after applying Windows job object controls",
        };
    }

    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, static_cast<DWORD>(request.spec.timeout_ms));
    bool timed_out = false;
    if (wait_result == WAIT_TIMEOUT) {
        timed_out = true;
        TerminateJobObject(job_handle, 124);
        WaitForSingleObject(process_info.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);

    CloseIfOpen(process_info.hThread);
    CloseIfOpen(process_info.hProcess);
    CloseIfOpen(stdout_write);
    stdout_write = nullptr;
    CloseIfOpen(stderr_write);
    stderr_write = nullptr;

    const auto stdout_text = ReadFileClipped(stdout_path, request.spec.output_limit_bytes);
    const auto stderr_text = ReadFileClipped(stderr_path, request.spec.output_limit_bytes);
    std::error_code remove_ec;
    std::filesystem::remove(stdout_path, remove_ec);
    std::filesystem::remove(stderr_path, remove_ec);

    CloseIfOpen(stdout_read);
    CloseIfOpen(stderr_read);
    CloseIfOpen(job_handle);

    return {
        .success = !timed_out && exit_code == 0,
        .exit_code = static_cast<int>(exit_code),
        .timed_out = timed_out,
        .duration_ms = ElapsedMs(started_at),
        .command_display = command_display,
        .stdout_text = ClipOutput(stdout_text, request.spec.output_limit_bytes),
        .stderr_text = ClipOutput(stderr_text, request.spec.output_limit_bytes),
        .error_code = timed_out ? "Timeout" : (exit_code == 0 ? "" : "ExternalProcessFailed"),
        .error_message = timed_out ? "CLI command timed out" : (exit_code == 0 ? "" : "CLI command exited with a non-zero status"),
    };
}

#else

std::vector<std::string> BuildPortableEnvironmentEntries(const CliSpec& spec) {
    std::vector<std::string> entries;
    std::unordered_set<std::string> seen;

    for (const auto& name : EffectiveEnvironmentAllowlist(spec)) {
        if (name.empty() || seen.contains(name)) {
            continue;
        }
        seen.insert(name);

        if (const auto value = ReadEnvironmentVariable(name); value.has_value()) {
            entries.push_back(name + "=" + *value);
        }
    }

    return entries;
}

std::vector<char*> BuildCStringVector(std::vector<std::string>& values) {
    std::vector<char*> output;
    output.reserve(values.size() + 1);
    for (auto& value : values) {
        output.push_back(value.data());
    }
    output.push_back(nullptr);
    return output;
}

bool SetNonBlocking(const int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

void AppendAvailablePipeOutput(const int fd, std::string& output, const std::size_t limit) {
    char buffer[4096];
    while (true) {
        const auto bytes_read = read(fd, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            if (output.size() < limit) {
                const auto remaining = limit - output.size();
                output.append(buffer, std::min<std::size_t>(static_cast<std::size_t>(bytes_read), remaining));
            }
            continue;
        }

        if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        return;
    }
}

void ApplyPortableResourceLimits(const CliSpec& spec) {
    if (spec.memory_limit_bytes > 0) {
        const rlim_t memory_limit = static_cast<rlim_t>(spec.memory_limit_bytes);
        const struct rlimit limit{memory_limit, memory_limit};
        (void)setrlimit(RLIMIT_AS, &limit);
    }

#ifdef RLIMIT_NPROC
    if (spec.max_processes > 0) {
        const rlim_t process_limit = static_cast<rlim_t>(spec.max_processes);
        const struct rlimit limit{process_limit, process_limit};
        (void)setrlimit(RLIMIT_NPROC, &limit);
    }
#endif

#ifdef RLIMIT_CPU
    if (spec.cpu_time_limit_seconds > 0) {
        const rlim_t cpu_limit = static_cast<rlim_t>(spec.cpu_time_limit_seconds);
        const struct rlimit limit{cpu_limit, cpu_limit};
        (void)setrlimit(RLIMIT_CPU, &limit);
    }
#endif

#ifdef RLIMIT_NOFILE
    if (spec.file_descriptor_limit > 0) {
        const rlim_t descriptor_limit = static_cast<rlim_t>(spec.file_descriptor_limit);
        const struct rlimit limit{descriptor_limit, descriptor_limit};
        (void)setrlimit(RLIMIT_NOFILE, &limit);
    }
#endif
}

int PortableExitCodeFromStatus(const int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
}

CliRunResult RunProcessPortable(
    const CliRunRequest& request,
    const std::filesystem::path& cwd,
    const std::vector<std::string>& args) {
    const auto started_at = std::chrono::steady_clock::now();
    auto command_display = BuildCommandLine(request.spec.binary, args);
    const auto resolved_binary = ResolveCommandPath(request.spec.binary).value_or(std::filesystem::path(request.spec.binary));

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "PipeCreateFailed",
            .error_message = "failed to create process output pipe",
        };
    }
    if (!SetNonBlocking(pipe_fds[0])) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "ProcessStartFailed",
            .error_message = "failed to configure process output pipe",
        };
    }

    auto arg_values = args;
    arg_values.insert(arg_values.begin(), request.spec.binary);
    auto argv = BuildCStringVector(arg_values);
    auto env_entries = BuildPortableEnvironmentEntries(request.spec);
    auto envp = BuildCStringVector(env_entries);

    const pid_t child_pid = fork();
    if (child_pid == -1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "ProcessStartFailed",
            .error_message = std::string("failed to fork process: ") + std::strerror(errno),
        };
    }

    if (child_pid == 0) {
        (void)setpgid(0, 0);
        close(pipe_fds[0]);
        (void)dup2(pipe_fds[1], STDOUT_FILENO);
        (void)dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        if (const int dev_null = open("/dev/null", O_RDONLY); dev_null != -1) {
            (void)dup2(dev_null, STDIN_FILENO);
            close(dev_null);
        }

        if (chdir(cwd.string().c_str()) != 0) {
            _exit(126);
        }

        ApplyPortableResourceLimits(request.spec);
        execve(resolved_binary.string().c_str(), argv.data(), envp.data());
        _exit(127);
    }

    (void)setpgid(child_pid, child_pid);
    close(pipe_fds[1]);

    std::string output;
    int status = 0;
    bool exited = false;
    bool timed_out = false;

    while (!exited) {
        AppendAvailablePipeOutput(pipe_fds[0], output, request.spec.output_limit_bytes);

        const pid_t wait_result = waitpid(child_pid, &status, WNOHANG);
        if (wait_result == child_pid) {
            exited = true;
            break;
        }

        if (wait_result == -1) {
            status = 127;
            exited = true;
            break;
        }

        if (ElapsedMs(started_at) >= request.spec.timeout_ms) {
            timed_out = true;
            kill(-child_pid, SIGKILL);
            (void)waitpid(child_pid, &status, 0);
            exited = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    AppendAvailablePipeOutput(pipe_fds[0], output, request.spec.output_limit_bytes);
    close(pipe_fds[0]);

    const int exit_code = timed_out ? 124 : PortableExitCodeFromStatus(status);
    return {
        .success = !timed_out && exit_code == 0,
        .exit_code = exit_code,
        .timed_out = timed_out,
        .duration_ms = ElapsedMs(started_at),
        .command_display = command_display,
        .stdout_text = ClipOutput(output, request.spec.output_limit_bytes),
        .error_code = timed_out ? "Timeout" : (exit_code == 0 ? "" : "ExternalProcessFailed"),
        .error_message = timed_out ? "CLI command timed out" : (exit_code == 0 ? "" : "CLI command exited with a non-zero status"),
    };
}

#endif

}  // namespace

std::string QuoteCommandForDisplay(const std::string& value) {
    if (!ContainsWhitespaceOrQuote(value)) {
        return value;
    }

    std::string quoted = "\"";
    std::size_t backslashes = 0;

    for (const char ch : value) {
        if (ch == '\\') {
            ++backslashes;
        } else if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back('"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, '\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }

    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

std::string CliHost::RenderTemplateValue(std::string value, const StringMap& arguments) {
    for (const auto& [key, argument_value] : arguments) {
        const auto placeholder = "{{" + key + "}}";
        std::size_t position = 0;
        while ((position = value.find(placeholder, position)) != std::string::npos) {
            value.replace(position, placeholder.size(), argument_value);
            position += argument_value.size();
        }
    }

    return value;
}

std::vector<std::string> CliHost::RenderArgs(const CliSpec& spec, const StringMap& arguments) {
    for (const auto& required_arg : spec.required_args) {
        if (!arguments.contains(required_arg)) {
            throw std::invalid_argument("missing required CLI argument: " + required_arg);
        }
    }

    std::vector<std::string> rendered_args;
    rendered_args.reserve(spec.args_template.size());

    for (const auto& arg_template : spec.args_template) {
        auto rendered = RenderTemplateValue(arg_template, arguments);
        if (rendered.empty() || rendered.find("{{") != std::string::npos) {
            continue;
        }
        rendered_args.push_back(std::move(rendered));
    }

    return rendered_args;
}

CliRunResult CliHost::run(const CliRunRequest& request) const {
    const auto started_at = std::chrono::steady_clock::now();

    try {
        auto cwd = NormalizeWorkspaceRoot(request.workspace_path);
        if (const auto it = request.arguments.find("cwd"); it != request.arguments.end()) {
            cwd = ResolveWorkspacePath(request.workspace_path, it->second);
        }

        if (!IsPathInsideWorkspace(request.workspace_path, cwd)) {
            return {
                .success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "WorkspaceEscapeDenied",
                .error_message = "CLI cwd must stay inside the active workspace",
            };
        }

        const auto args = RenderArgs(request.spec, request.arguments);

#ifdef _WIN32
        return RedactCliRunResult(RunProcessWindows(request, cwd, args), request.arguments);
#else
        return RedactCliRunResult(RunProcessPortable(request, cwd, args), request.arguments);
#endif
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "InvalidArguments",
            .error_message = error.what(),
        };
    }
}

}  // namespace agentos
