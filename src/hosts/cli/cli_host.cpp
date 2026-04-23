#include "hosts/cli/cli_host.hpp"

#include "utils/command_utils.hpp"
#include "utils/path_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
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
#endif

namespace agentos {

namespace {

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

bool ContainsWhitespaceOrQuote(const std::string& value) {
    return value.empty() || std::any_of(value.begin(), value.end(), [](const unsigned char ch) {
        return std::isspace(ch) != 0 || ch == '"';
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

std::string ClipOutput(const std::string& value, const std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }

    return value.substr(0, limit) + "\n[agentos: output truncated]";
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

std::vector<char> BuildWindowsEnvironmentBlock(const CliSpec& spec) {
    std::map<std::string, std::pair<std::string, std::string>> values;

    for (const auto& name : EffectiveEnvironmentAllowlist(spec)) {
        if (name.empty()) {
            continue;
        }

        if (const auto value = ReadEnvironmentVariable(name); value.has_value()) {
            values[ToUpper(name)] = {name, *value};
        }
    }

    std::vector<char> block;
    for (const auto& [unused_key, entry] : values) {
        (void)unused_key;
        const auto line = entry.first + "=" + entry.second;
        block.insert(block.end(), line.begin(), line.end());
        block.push_back('\0');
    }
    block.push_back('\0');
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

    if (!CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0)) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "PipeCreateFailed",
            .error_message = "failed to create process output pipes",
        };
    }

    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(STARTUPINFOA);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stderr_write;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    auto command_display = BuildCommandLine(request.spec.binary, args);
    auto process_command_line = BuildWindowsProcessCommandLine(request.spec.binary, args);
    auto mutable_command_line = process_command_line;
    auto cwd_string = cwd.string();
    auto environment_block = BuildWindowsEnvironmentBlock(request.spec);

    const BOOL created = CreateProcessA(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        environment_block.data(),
        cwd_string.c_str(),
        &startup_info,
        &process_info);

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_display,
            .error_code = "ProcessStartFailed",
            .error_message = "failed to start process",
        };
    }

    std::string stdout_text;
    std::string stderr_text;
    std::thread stdout_thread(ReadPipeToString, stdout_read, std::ref(stdout_text), request.spec.output_limit_bytes);
    std::thread stderr_thread(ReadPipeToString, stderr_read, std::ref(stderr_text), request.spec.output_limit_bytes);

    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, static_cast<DWORD>(request.spec.timeout_ms));
    bool timed_out = false;
    if (wait_result == WAIT_TIMEOUT) {
        timed_out = true;
        TerminateProcess(process_info.hProcess, 124);
        WaitForSingleObject(process_info.hProcess, INFINITE);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    if (stdout_thread.joinable()) {
        stdout_thread.join();
    }
    if (stderr_thread.joinable()) {
        stderr_thread.join();
    }

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);

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

std::string BuildPortableEnvironmentPrefix(const CliSpec& spec) {
    std::ostringstream output;
    output << "env -i";

    std::unordered_set<std::string> seen;
    for (const auto& name : EffectiveEnvironmentAllowlist(spec)) {
        if (name.empty() || seen.contains(name)) {
            continue;
        }
        seen.insert(name);

        if (const auto value = ReadEnvironmentVariable(name); value.has_value()) {
            output << ' ' << QuoteCommandForDisplay(name + "=" + *value);
        }
    }

    return output.str();
}

CliRunResult RunProcessPortable(
    const CliRunRequest& request,
    const std::filesystem::path& cwd,
    const std::vector<std::string>& args) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto command_line = "cd " + QuoteCommandForDisplay(cwd.string()) + " && " +
                              BuildPortableEnvironmentPrefix(request.spec) + " " +
                              BuildCommandLine(request.spec.binary, args) + " 2>&1";

    FILE* pipe = popen(command_line.c_str(), "r");
    if (!pipe) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .command_display = command_line,
            .error_code = "ProcessStartFailed",
            .error_message = "failed to start process",
        };
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        if (output.size() < request.spec.output_limit_bytes) {
            output.append(buffer);
        }
    }

    const int status = pclose(pipe);
    return {
        .success = status == 0,
        .exit_code = status,
        .duration_ms = ElapsedMs(started_at),
        .command_display = command_line,
        .stdout_text = ClipOutput(output, request.spec.output_limit_bytes),
        .error_code = status == 0 ? "" : "ExternalProcessFailed",
        .error_message = status == 0 ? "" : "CLI command exited with a non-zero status",
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
        return RunProcessWindows(request, cwd, args);
#else
        return RunProcessPortable(request, cwd, args);
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
