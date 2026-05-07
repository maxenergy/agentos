#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <optional>
#ifndef _WIN32
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <pty.h>
#include <thread>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace(const std::string& name) {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_cli_tests" / name;
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

void CreateAutoDevSkillPackFixture(const std::filesystem::path& root) {
    const std::vector<std::string> steps = {
        "00-understand-system",
        "01-grill-requirements",
        "02-spec-freeze",
        "03-impact-analysis",
        "04-task-slice",
        "05-goal-pack",
        "07-verify-loop",
        "08-goal-review",
    };
    for (const auto& step : steps) {
        const auto dir = root / step;
        std::filesystem::create_directories(dir);
        std::ofstream skill(dir / "SKILL.md", std::ios::binary);
        skill << "---\nname: " << step << "\n---\n# " << step << "\n";
    }
}

std::string QuoteShellArg(const std::string& value) {
#ifdef _WIN32
    if (value.find_first_of(" \t\n\"&<>|^") == std::string::npos) {
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
#else
    if (value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-=./:") == std::string::npos) {
        return value;
    }

    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
#endif
}

void InitGitRepoForCliTest(const std::filesystem::path& repo) {
    std::filesystem::create_directories(repo);
    (void)std::system((std::string("git -C ") + QuoteShellArg(repo.string()) + " init >/dev/null 2>&1").c_str());
    (void)std::system((std::string("git -C ") + QuoteShellArg(repo.string()) + " config user.email test@example.com").c_str());
    (void)std::system((std::string("git -C ") + QuoteShellArg(repo.string()) + " config user.name Test").c_str());
    {
        std::ofstream readme(repo / "README.md", std::ios::binary);
        readme << "fixture\n";
    }
    (void)std::system((std::string("git -C ") + QuoteShellArg(repo.string()) + " add README.md").c_str());
    (void)std::system((std::string("git -C ") + QuoteShellArg(repo.string()) + " commit -m initial >/dev/null 2>&1").c_str());
}

int DecodeProcessStatus(const int status) {
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
#endif
}

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult RunAgentos(const std::filesystem::path& workspace, const std::vector<std::string>& args) {
    std::ostringstream command;
#ifdef _WIN32
    command << "cd /d " << QuoteShellArg(workspace.string()) << " && ";
#else
    command << "cd " << QuoteShellArg(workspace.string()) << " && ";
#endif
    command << QuoteShellArg(AGENTOS_CLI_TEST_EXE);
    for (const auto& arg : args) {
        command << ' ' << QuoteShellArg(arg);
    }
    command << " 2>&1";

#ifdef _WIN32
    FILE* pipe = _popen(command.str().c_str(), "r");
#else
    FILE* pipe = popen(command.str().c_str(), "r");
#endif
    if (!pipe) {
        return {.exit_code = -1, .output = "failed to open process"};
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    return {.exit_code = DecodeProcessStatus(status), .output = std::move(output)};
}

// Spawns the built agentos binary with stdin redirected from a workspace-local
// temp file containing `stdin_text`.  Used by the interactive-login test —
// _popen with mode "r" only opens the child's stdout, so we drive stdin via a
// shell-level `< file` redirect that works on both cmd.exe and POSIX sh.
CommandResult RunAgentosWithStdin(
    const std::filesystem::path& workspace,
    const std::vector<std::string>& args,
    const std::string& stdin_text) {
    const auto stdin_path = workspace / "stdin_input.txt";
    {
        std::ofstream stream(stdin_path, std::ios::binary);
        stream << stdin_text;
    }

    std::ostringstream command;
#ifdef _WIN32
    command << "cd /d " << QuoteShellArg(workspace.string()) << " && ";
#else
    command << "cd " << QuoteShellArg(workspace.string()) << " && ";
#endif
    command << QuoteShellArg(AGENTOS_CLI_TEST_EXE);
    for (const auto& arg : args) {
        command << ' ' << QuoteShellArg(arg);
    }
    command << " < " << QuoteShellArg(stdin_path.string());
    command << " 2>&1";

#ifdef _WIN32
    FILE* pipe = _popen(command.str().c_str(), "r");
#else
    FILE* pipe = popen(command.str().c_str(), "r");
#endif
    if (!pipe) {
        return {.exit_code = -1, .output = "failed to open process"};
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    return {.exit_code = DecodeProcessStatus(status), .output = std::move(output)};
}

#ifndef _WIN32
CommandResult RunAgentosInPtyWithInput(
    const std::filesystem::path& workspace,
    const std::vector<std::string>& args,
    const std::string& stdin_text) {
    std::vector<std::string> input_chunks;
    std::string current_chunk;
    for (const char ch : stdin_text) {
        current_chunk.push_back(ch);
        if (ch == '\n') {
            input_chunks.push_back(std::move(current_chunk));
            current_chunk.clear();
        }
    }
    if (!current_chunk.empty()) {
        input_chunks.push_back(std::move(current_chunk));
    }

    int master_fd = -1;
    const pid_t pid = forkpty(&master_fd, nullptr, nullptr, nullptr);
    if (pid < 0) {
        return {.exit_code = -1, .output = "forkpty failed"};
    }
    if (pid == 0) {
        std::filesystem::current_path(workspace);
        std::vector<std::string> owned_args;
        owned_args.emplace_back(AGENTOS_CLI_TEST_EXE);
        owned_args.insert(owned_args.end(), args.begin(), args.end());
        std::vector<char*> argv;
        argv.reserve(owned_args.size() + 1);
        for (auto& arg : owned_args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        execv(AGENTOS_CLI_TEST_EXE, argv.data());
        _exit(127);
    }

    const int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
    }

    std::string output;
    std::size_t next_input_chunk = 0;
    std::size_t prompt_search_start = 0;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        char buffer[4096];
        while (true) {
            const ssize_t count = read(master_fd, buffer, sizeof(buffer));
            if (count > 0) {
                output.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            break;
        }
        while (next_input_chunk < input_chunks.size()) {
            const auto prompt_pos = output.find("agentos> ", prompt_search_start);
            if (prompt_pos == std::string::npos) {
                break;
            }
            prompt_search_start = prompt_pos + 9;
            const auto& chunk = input_chunks[next_input_chunk++];
            (void)write(master_fd, chunk.data(), chunk.size());
        }
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    close(master_fd);
    if (next_input_chunk < input_chunks.size()) {
        kill(pid, SIGTERM);
    }
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    return {.exit_code = DecodeProcessStatus(status), .output = std::move(output)};
}
#endif

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string ExtractTokenValue(const std::string& text, const std::string& key) {
    const auto start = text.find(key);
    if (start == std::string::npos) {
        return "";
    }
    const auto value_start = start + key.size();
    const auto value_end = text.find_first_of(" \r\n", value_start);
    return text.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
}

std::string TrimAscii(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string ExtractLineValue(const std::string& text, const std::string& key) {
    const auto start = text.find(key);
    if (start == std::string::npos) {
        return {};
    }
    const auto value_start = start + key.size();
    const auto value_end = text.find_first_of("\r\n", value_start);
    return TrimAscii(text.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start));
}

std::optional<std::string> ReadEnvForTest(const std::string& name) {
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
    const char* value = std::getenv(name.c_str());
    if (!value) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

void SetEnvForTest(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

char PathListSeparatorForTest() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

void WriteCurlTokenFixture(const std::filesystem::path& bin_dir) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "curl.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "echo {\"access_token\":\"cli-access\",\"refresh_token\":\"cli-refresh\",\"token_type\":\"Bearer\",\"expires_in\":900}\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "curl";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "printf '%s\\n' '{\"access_token\":\"cli-access\",\"refresh_token\":\"cli-refresh\",\"token_type\":\"Bearer\",\"expires_in\":900}'\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
}

void WriteMainRouteActionCurlFixture(const std::filesystem::path& bin_dir,
                                     const std::filesystem::path& counter_path) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "curl.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "set COUNTER=" << counter_path.string() << "\n"
        << "if not exist \"%COUNTER%\" echo 0>\"%COUNTER%\"\n"
        << "set /p COUNT=<\"%COUNTER%\"\n"
        << "if \"%COUNT%\"==\"0\" (\n"
        << "  echo 1>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"{\\\"agentos_route_action\\\":{\\\"action\\\":\\\"call_capability\\\",\\\"target_kind\\\":\\\"skill\\\",\\\"target\\\":\\\"host_info\\\",\\\"brief\\\":\\\"GOAL: inspect host info for REPL route action test. FORMAT: concise JSON-backed summary. SUCCESS: host_info skill runs.\\\",\\\"mode\\\":\\\"sync\\\",\\\"arguments\\\":{\\\"objective\\\":\\\"inspect host info\\\"}}}\"}}]}\n"
        << ") else (\n"
        << "  echo 2>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"route action synthesis complete\"}}]}\n"
        << ")\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "curl";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "counter=" << QuoteShellArg(counter_path.string()) << "\n"
        << "if [ ! -f \"$counter\" ]; then printf '0\\n' > \"$counter\"; fi\n"
        << "count=$(cat \"$counter\")\n"
        << "if [ \"$count\" = \"0\" ]; then\n"
        << "  printf '1\\n' > \"$counter\"\n"
        << "  cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"{\"agentos_route_action\":{\"action\":\"call_capability\",\"target_kind\":\"skill\",\"target\":\"host_info\",\"brief\":\"GOAL: inspect host info for REPL route action test. FORMAT: concise JSON-backed summary. SUCCESS: host_info skill runs.\",\"mode\":\"sync\",\"arguments\":{\"objective\":\"inspect host info\"}}}"}}]})"
        << "\nJSON\n"
        << "else\n"
        << "  printf '2\\n' > \"$counter\"\n"
        << "  cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"route action synthesis complete"}}]})"
        << "\nJSON\n"
        << "fi\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
}

void WriteMainRouteActionMissingInputCurlFixture(const std::filesystem::path& bin_dir,
                                                const std::filesystem::path& counter_path) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "curl.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "set COUNTER=" << counter_path.string() << "\n"
        << "if not exist \"%COUNTER%\" echo 0>\"%COUNTER%\"\n"
        << "set /p COUNT=<\"%COUNTER%\"\n"
        << "if \"%COUNT%\"==\"0\" (\n"
        << "  echo 1>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"{\\\"agentos_route_action\\\":{\\\"action\\\":\\\"call_capability\\\",\\\"target_kind\\\":\\\"skill\\\",\\\"target\\\":\\\"news_search\\\",\\\"brief\\\":\\\"GOAL: search news.\\\",\\\"mode\\\":\\\"sync\\\",\\\"arguments\\\":{}}}\"}}]}\n"
        << ") else (\n"
        << "  echo 2>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"route action validation synthesis complete\"}}]}\n"
        << ")\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "curl";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "counter=" << QuoteShellArg(counter_path.string()) << "\n"
        << "if [ ! -f \"$counter\" ]; then printf '0\\n' > \"$counter\"; fi\n"
        << "count=$(cat \"$counter\")\n"
        << "if [ \"$count\" = \"0\" ]; then\n"
        << "  printf '1\\n' > \"$counter\"\n"
        << "  cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"{\"agentos_route_action\":{\"action\":\"call_capability\",\"target_kind\":\"skill\",\"target\":\"news_search\",\"brief\":\"GOAL: search news.\",\"mode\":\"sync\",\"arguments\":{}}}"}}]})"
        << "\nJSON\n"
        << "else\n"
        << "  printf '2\\n' > \"$counter\"\n"
        << "  cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"route action validation synthesis complete"}}]})"
        << "\nJSON\n"
        << "fi\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
}

void WriteMainRouteActionContextCurlFixture(const std::filesystem::path& bin_dir,
                                           const std::filesystem::path& counter_path) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "curl.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "set COUNTER=" << counter_path.string() << "\n"
        << "if not exist \"%COUNTER%\" echo 0>\"%COUNTER%\"\n"
        << "set /p COUNT=<\"%COUNTER%\"\n"
        << "if \"%COUNT%\"==\"0\" (\n"
        << "  echo 1>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"{\\\"agentos_route_action\\\":{\\\"action\\\":\\\"call_capability\\\",\\\"target_kind\\\":\\\"skill\\\",\\\"target\\\":\\\"news_search\\\",\\\"brief\\\":\\\"GOAL: search news.\\\",\\\"mode\\\":\\\"sync\\\",\\\"arguments\\\":{}}}\"}}]}\n"
        << ") else if \"%COUNT%\"==\"1\" (\n"
        << "  echo 2>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"please provide query\"}}]}\n"
        << ") else if \"%COUNT%\"==\"2\" (\n"
        << "  echo 3>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"{\\\"agentos_route_action\\\":{\\\"action\\\":\\\"call_capability\\\",\\\"target_kind\\\":\\\"skill\\\",\\\"target\\\":\\\"news_search\\\",\\\"brief\\\":\\\"GOAL: search news with supplied query.\\\",\\\"mode\\\":\\\"sync\\\",\\\"arguments\\\":{\\\"query\\\":\\\"AI browser\\\"}}}\"}}]}\n"
        << ") else (\n"
        << "  echo 4>\"%COUNTER%\"\n"
        << "  echo {\"choices\":[{\"message\":{\"content\":\"context ok\"}}]}\n"
        << ")\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "curl";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "counter=" << QuoteShellArg(counter_path.string()) << "\n"
        << "if [ ! -f \"$counter\" ]; then printf '0\\n' > \"$counter\"; fi\n"
        << "count=$(cat \"$counter\")\n"
        << "body=''\n"
        << "prev=''\n"
        << "for arg in \"$@\"; do\n"
        << "  if [ \"$prev\" = '-d' ]; then body=${arg#@}; fi\n"
        << "  prev=$arg\n"
        << "done\n"
        << "if [ \"$count\" = \"0\" ]; then\n"
        << "  printf '1\\n' > \"$counter\"\n"
        << "  cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"{\"agentos_route_action\":{\"action\":\"call_capability\",\"target_kind\":\"skill\",\"target\":\"news_search\",\"brief\":\"GOAL: search news.\",\"mode\":\"sync\",\"arguments\":{}}}"}}]})"
        << "\nJSON\n"
        << "elif [ \"$count\" = \"1\" ]; then\n"
        << "  printf '2\\n' > \"$counter\"\n"
        << "  cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"please provide query"}}]})"
        << "\nJSON\n"
        << "elif [ \"$count\" = \"2\" ]; then\n"
        << "  printf '3\\n' > \"$counter\"\n"
        << "  if grep -q 'AGENTOS ROUTE ACTION RESULT' \"$body\"; then\n"
        << "    cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"context leaked internal route result"}}]})"
        << "\nJSON\n"
        << "  elif grep -q 'User: search news through main' \"$body\" && grep -q 'Assistant: please provide query' \"$body\" && grep -q 'PENDING AGENTOS ROUTE ACTION' \"$body\" && grep -q 'news_search' \"$body\" && grep -q 'query' \"$body\"; then\n"
        << "    cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"{\"agentos_route_action\":{\"action\":\"call_capability\",\"target_kind\":\"skill\",\"target\":\"news_search\",\"brief\":\"GOAL: search news with supplied query.\",\"mode\":\"sync\",\"arguments\":{\"query\":\"AI browser\"}}}"}}]})"
        << "\nJSON\n"
        << "  else\n"
        << "    cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"context missing pending route action"}}]})"
        << "\nJSON\n"
        << "  fi\n"
        << "else\n"
        << "  printf '4\\n' > \"$counter\"\n"
        << "  if grep -q 'PENDING AGENTOS ROUTE ACTION' \"$body\"; then\n"
        << "    cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"pending leaked after success"}}]})"
        << "\nJSON\n"
        << "  else\n"
        << "    cat <<'JSON'\n"
        << R"({"choices":[{"message":{"content":"context ok"}}]})"
        << "\nJSON\n"
        << "  fi\n"
        << "fi\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
}

std::filesystem::path WriteAutoDevCodexCliFixture(const std::filesystem::path& bin_dir) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "autodev_codex_fixture.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "type CON > NUL\n"
        << "echo codex fixture update>> README.md\n"
        << "echo fixture codex completed\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "autodev_codex_fixture";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "cat >/dev/null\n"
        << "printf '%s\\n' 'codex fixture update' >> README.md\n"
        << "printf '%s\\n' 'fixture codex completed'\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
    return fixture_path;
}

std::filesystem::path WriteLongAutoDevCodexCliFixture(
    const std::filesystem::path& bin_dir,
    const std::filesystem::path& marker_path) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    const auto fixture_path = bin_dir / "autodev_long_codex_fixture.cmd";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "@echo off\n"
        << "type CON > NUL\n"
        << "echo started>" << marker_path.string() << "\n"
        << "ping -n 11 127.0.0.1 > NUL\n"
        << "echo long fixture completed>> README.md\n"
        << "echo completed\n"
        << "exit /b 0\n";
#else
    const auto fixture_path = bin_dir / "autodev_long_codex_fixture";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "cat >/dev/null\n"
        << "printf '%s\\n' started > " << QuoteShellArg(marker_path.string()) << "\n"
        << "sleep 10\n"
        << "printf '%s\\n' 'long fixture completed' >> README.md\n"
        << "printf '%s\\n' completed\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif
    return fixture_path;
}

#ifndef _WIN32
void WriteSleepingCodexFixture(const std::filesystem::path& bin_dir) {
    std::filesystem::create_directories(bin_dir);
    const auto fixture_path = bin_dir / "codex";
    std::ofstream output(fixture_path, std::ios::binary);
    output
        << "#!/usr/bin/env sh\n"
        << "sleep 1\n"
        << "printf '%s\\n' '{\"ok\":false}'\n"
        << "exit 1\n";
    output.close();
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
}
#endif

void TestAgentsCommand() {
    const auto workspace = FreshWorkspace("agents");
    const auto result = RunAgentos(workspace, {"agents"});
    Expect(result.exit_code == 0, "agents command should succeed");
    Expect(result.output.find("local_planner") != std::string::npos, "agents command should list local_planner");
    Expect(result.output.find("codex_cli") != std::string::npos, "agents command should list codex_cli");
}

void TestSkillsCommandShowsRepoAgentSkills() {
    const auto workspace = FreshWorkspace("skills_repo_agent_skills");
    const auto skill_dir = workspace / ".agents" / "skills" / "sample-agent-skill";
    std::filesystem::create_directories(skill_dir);
    {
        std::ofstream output(skill_dir / "SKILL.md", std::ios::binary);
        output << "# sample-agent-skill\n";
    }

    const auto result = RunAgentosWithStdin(workspace, {"interactive"}, "skills\nexit\n");
    Expect(result.exit_code == 0, "skills command should succeed");
    Expect(result.output.find("可用 skill") != std::string::npos,
        "skills command should still list runtime skills");
    Expect(result.output.find("仓库级 agent skills") != std::string::npos,
        "skills command should list repo-level agent skills separately");
    Expect(result.output.find("sample-agent-skill") != std::string::npos,
        "skills command should include repo-level agent skill names");
    Expect(result.output.find("不是 `run` 调用的 runtime skill") != std::string::npos,
        "skills command should explain agent skills are not runtime skills");
}

void TestPluginsCommand() {
    const auto workspace = FreshWorkspace("plugins");
    std::filesystem::create_directories(workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
    const auto health_args_template = "/d,/s,/c,exit 0";
    const auto failing_health_args_template = "/d,/s,/c,exit 7";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
    const auto health_args_template = "-c,exit 0";
    const auto failing_health_args_template = "-c,exit 7";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "cli_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "cli_plugin" << '\t'
            << "CLI plugin health fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << health_args_template << '\t'
            << "1000"
            << '\n';
    }
    {
#ifdef _WIN32
        const auto json_args_template = R"(  "args_template": ["/d", "/s", "/c", "echo {{message}}"],)";
        const auto json_health_args_template = R"(  "health_args_template": ["/d", "/s", "/c", "exit 0"])";
#else
        const auto json_args_template = R"(  "args_template": ["-c", "printf '%s\\n' \"{{message}}\""],)";
        const auto json_health_args_template = R"(  "health_args_template": ["-c", "exit 0"])";
#endif
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "json_cli_plugin.json", std::ios::binary);
        spec_file
            << "{\n"
            << R"(  "manifest_version": "plugin.v1",)" << '\n'
            << R"(  "name": "json_cli_plugin",)" << '\n'
            << R"(  "description": "JSON CLI plugin health fixture.",)" << '\n'
            << R"(  "binary": ")" << binary << R"(",)" << '\n'
            << json_args_template << '\n'
            << R"(  "required_args": ["message"],)" << '\n'
            << R"(  "protocol": "stdio-json-v0",)" << '\n'
            << R"(  "input_schema_json": {"type": "object"},)" << '\n'
            << R"(  "output_schema_json": {"type": "object"},)" << '\n'
            << R"(  "risk_level": "low",)" << '\n'
            << R"(  "permissions": ["process.spawn"],)" << '\n'
            << R"(  "timeout_ms": 3000,)" << '\n'
            << R"(  "health_timeout_ms": 1000,)" << '\n'
            << json_health_args_template << '\n'
            << "}\n";
    }

    const auto result = RunAgentos(workspace, {"plugins"});
    Expect(result.exit_code == 0, "plugins command should succeed");
    Expect(result.output.find("cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins command should list loaded plugin specs");
    Expect(result.output.find("json_cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins command should list loaded JSON plugin specs");
    Expect(result.output.find("source=") != std::string::npos && result.output.find("cli_plugin.tsv") != std::string::npos,
        "plugins command should include loaded plugin source file paths");
    Expect(result.output.find("json_cli_plugin.json") != std::string::npos && result.output.find("line=1") != std::string::npos,
        "plugins command should include loaded JSON plugin source line");
    Expect(result.output.find("lifecycle_mode=oneshot") != std::string::npos,
        "plugins command should include plugin lifecycle mode");
    Expect(result.output.find("startup_timeout_ms=3000") != std::string::npos,
        "plugins command should include plugin startup timeout");
    Expect(result.output.find("idle_timeout_ms=30000") != std::string::npos,
        "plugins command should include plugin idle timeout");
    Expect(result.output.find("healthy=true") != std::string::npos,
        "plugins command should include plugin health");

    const auto validate = RunAgentos(workspace, {"plugins", "validate"});
    Expect(validate.exit_code == 0, "plugins validate should succeed when every plugin spec is valid");
    Expect(validate.output.find("cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins validate should list valid plugin specs");
    Expect(validate.output.find("json_cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins validate should list valid JSON plugin specs");
    Expect(validate.output.find("source=") != std::string::npos && validate.output.find("cli_plugin.tsv") != std::string::npos,
        "plugins validate should include loaded plugin source file paths");
    Expect(validate.output.find("valid=true") != std::string::npos,
        "plugins validate should report valid loaded plugin specs");

    const auto health = RunAgentos(workspace, {"plugins", "health"});
    Expect(health.exit_code == 0, "plugins health should succeed when every loaded plugin is healthy");
    Expect(health.output.find("cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should print the same plugin health listing");
    Expect(health.output.find("json_cli_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should print JSON plugin health listing");
    Expect(health.output.find("source=") != std::string::npos && health.output.find("json_cli_plugin.json") != std::string::npos,
        "plugins health should include loaded plugin source file paths");

    const auto inspect = RunAgentos(workspace, {"plugins", "inspect", "name=cli_plugin"});
    Expect(inspect.exit_code == 0, "plugins inspect should succeed for a loaded plugin");
    Expect(inspect.output.find("name=cli_plugin") != std::string::npos,
        "plugins inspect should print the plugin name");
    Expect(inspect.output.find("protocol=stdio-json-v0") != std::string::npos,
        "plugins inspect should print the plugin protocol");
    Expect(inspect.output.find("required_args=message") != std::string::npos,
        "plugins inspect should print required args");
    Expect(inspect.output.find("permissions=process.spawn") != std::string::npos,
        "plugins inspect should print permissions");
    Expect(inspect.output.find("lifecycle_mode=oneshot") != std::string::npos,
        "plugins inspect should print lifecycle mode");
    Expect(inspect.output.find("isolation_profile=workspace-paths") != std::string::npos,
        "plugins inspect should print the plugin isolation profile");
    Expect(inspect.output.find("resource_limits_configured=false") != std::string::npos,
        "plugins inspect should state whether plugin resource limits are configured");
    Expect(inspect.output.find("valid=true") != std::string::npos,
        "plugins inspect should report loaded plugin validity");

    const auto inspect_health = RunAgentos(workspace, {"plugins", "inspect", "name=cli_plugin", "health=true"});
    Expect(inspect_health.exit_code == 0, "plugins inspect health should succeed for a healthy plugin");
    Expect(inspect_health.output.find("healthy=true") != std::string::npos,
        "plugins inspect health should report healthy plugins");
    Expect(inspect_health.output.find("command_available=true") != std::string::npos,
        "plugins inspect health should report command availability");

    const auto missing_inspect = RunAgentos(workspace, {"plugins", "inspect", "name=missing_plugin"});
    Expect(missing_inspect.exit_code != 0, "plugins inspect should fail for a missing plugin");
    Expect(missing_inspect.output.find("plugin_not_found name=missing_plugin") != std::string::npos,
        "plugins inspect should report the missing plugin name");

    const auto lifecycle = RunAgentos(workspace, {"plugins", "lifecycle"});
    Expect(lifecycle.exit_code == 0, "plugins lifecycle should succeed for valid plugin specs");
    Expect(lifecycle.output.find("plugin_lifecycle name=cli_plugin lifecycle_mode=oneshot") != std::string::npos,
        "plugins lifecycle should list per-plugin lifecycle settings");
    Expect(lifecycle.output.find("isolation_profile=workspace-paths") != std::string::npos,
        "plugins lifecycle should report each plugin isolation profile");
    Expect(lifecycle.output.find("resource_limits_configured=false") != std::string::npos,
        "plugins lifecycle should state when resource limits are not configured");
    Expect(lifecycle.output.find("plugin_lifecycle_summary total=2 oneshot=2 persistent=0") != std::string::npos,
        "plugins lifecycle should summarize lifecycle counts");

    const auto lifecycle_workspace = FreshWorkspace("plugins_lifecycle");
    std::filesystem::create_directories(lifecycle_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream options_file(lifecycle_workspace / "runtime" / "plugin_host.tsv", std::ios::binary);
        options_file << "max_persistent_sessions\t3\n";
    }
    {
        std::ofstream spec_file(lifecycle_workspace / "runtime" / "plugin_specs" / "persistent_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "persistent_plugin" << '\t'
            << "Persistent plugin lifecycle fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "json-rpc-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "1048576" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "3000" << '\t'
            << "workspace" << '\t'
            << "persistent" << '\t'
            << "1500" << '\t'
            << "60000" << '\t'
            << "5"
            << '\n';
    }
    const auto persistent_lifecycle = RunAgentos(lifecycle_workspace, {"plugins", "lifecycle"});
    Expect(persistent_lifecycle.exit_code == 0, "plugins lifecycle should not require starting persistent plugin processes");
    Expect(persistent_lifecycle.output.find("plugin_lifecycle name=persistent_plugin lifecycle_mode=persistent protocol=json-rpc-v0") != std::string::npos,
        "plugins lifecycle should report persistent JSON-RPC plugins");
    Expect(persistent_lifecycle.output.find("sandbox_mode=workspace isolation_profile=workspace-paths+process-resource-limits resource_limits_configured=true") != std::string::npos,
        "plugins lifecycle should report persistent plugin sandbox and resource-limit isolation profile");
    Expect(persistent_lifecycle.output.find("persistent=1") != std::string::npos,
        "plugins lifecycle summary should count persistent plugins");
    Expect(persistent_lifecycle.output.find("max_persistent_sessions=3") != std::string::npos,
        "plugins lifecycle summary should report workspace-configured persistent session cap");
    Expect(persistent_lifecycle.output.find("pool_size=5") != std::string::npos,
        "plugins lifecycle should include the per-plugin pool_size manifest field");
    Expect(persistent_lifecycle.output.find("effective_pool_size=3") != std::string::npos,
        "plugins lifecycle should report the pool size after the global persistent-session cap");
    Expect(persistent_lifecycle.output.find("pool_policy=per_plugin_workspace_binary_lru") != std::string::npos,
        "plugins lifecycle should report the process-pool eviction policy");
    Expect(persistent_lifecycle.output.find("scope=process persistence=none") != std::string::npos,
        "plugins lifecycle summary should state that session admin is process scoped");

    const auto persistent_inspect = RunAgentos(lifecycle_workspace, {"plugins", "inspect", "name=persistent_plugin"});
    Expect(persistent_inspect.exit_code == 0, "plugins inspect should succeed for persistent plugin specs");
    Expect(persistent_inspect.output.find("max_persistent_sessions=3") != std::string::npos,
        "plugins inspect should report the workspace persistent-session cap");
    Expect(persistent_inspect.output.find("effective_pool_size=3") != std::string::npos,
        "plugins inspect should report the capped effective persistent pool size");
    Expect(persistent_inspect.output.find("pool_policy=per_plugin_workspace_binary_lru") != std::string::npos,
        "plugins inspect should report the process-pool eviction policy");
    Expect(persistent_inspect.output.find("isolation_profile=workspace-paths+process-resource-limits") != std::string::npos,
        "plugins inspect should report combined workspace/resource-limit isolation");
    Expect(persistent_inspect.output.find("resource_limits_configured=true") != std::string::npos,
        "plugins inspect should state when plugin resource limits are configured");
    {
        std::ofstream options_file(lifecycle_workspace / "runtime" / "plugin_host.tsv", std::ios::binary);
        options_file << "max_persistent_sessions\tinvalid\n";
    }
    const auto invalid_config_inspect = RunAgentos(lifecycle_workspace, {"plugins", "inspect", "name=persistent_plugin"});
    Expect(invalid_config_inspect.exit_code != 0,
        "plugins inspect should fail when plugin_host.tsv contains invalid pool policy");
    Expect(invalid_config_inspect.output.find("plugin_host_config_diagnostic line=1") != std::string::npos,
        "plugins inspect should report plugin host config diagnostics");

    const auto invalid_lifecycle_workspace = FreshWorkspace("plugins_lifecycle_invalid");
    std::filesystem::create_directories(invalid_lifecycle_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream options_file(invalid_lifecycle_workspace / "runtime" / "plugin_host.tsv", std::ios::binary);
        options_file << "max_persistent_sessions\tinvalid\n";
    }
    const auto invalid_lifecycle = RunAgentos(invalid_lifecycle_workspace, {"plugins", "lifecycle"});
    Expect(invalid_lifecycle.exit_code != 0, "plugins lifecycle should fail when plugin host config is invalid");
    Expect(invalid_lifecycle.output.find("plugin_host_config_diagnostic line=1") != std::string::npos,
        "plugins lifecycle should print plugin host config diagnostics");
    Expect(invalid_lifecycle.output.find("config_diagnostics=1") != std::string::npos,
        "plugins lifecycle summary should count plugin host config diagnostics");

    const auto unhealthy_workspace = FreshWorkspace("plugins_unhealthy");
    std::filesystem::create_directories(unhealthy_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(unhealthy_workspace / "runtime" / "plugin_specs" / "missing_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "missing_plugin" << '\t'
            << "Missing plugin binary fixture." << '\t'
            << "agentos_missing_plugin_binary_for_cli_test" << '\t'
            << "{{message}}" << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto unhealthy = RunAgentos(unhealthy_workspace, {"plugins", "health"});
    Expect(unhealthy.exit_code != 0, "plugins health should fail when any loaded plugin is unhealthy");
    Expect(unhealthy.output.find("missing_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should list unhealthy plugins");
    Expect(unhealthy.output.find("healthy=false") != std::string::npos,
        "plugins health should report unhealthy plugin state");

    const auto unhealthy_validate = RunAgentos(unhealthy_workspace, {"plugins", "validate"});
    Expect(unhealthy_validate.exit_code == 0,
        "plugins validate should not fail solely because a valid plugin binary is unavailable");
    Expect(unhealthy_validate.output.find("missing_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins validate should still list valid specs whose binary is unavailable");

    const auto unhealthy_inspect = RunAgentos(unhealthy_workspace, {"plugins", "inspect", "name=missing_plugin", "health=true"});
    Expect(unhealthy_inspect.exit_code != 0, "plugins inspect health should fail for an unhealthy plugin");
    Expect(unhealthy_inspect.output.find("name=missing_plugin") != std::string::npos,
        "plugins inspect health should print the unhealthy plugin name");
    Expect(unhealthy_inspect.output.find("healthy=false") != std::string::npos,
        "plugins inspect health should report unhealthy plugins");
    Expect(unhealthy_inspect.output.find("command_available=false") != std::string::npos,
        "plugins inspect health should report unavailable commands");

    const auto failed_probe_workspace = FreshWorkspace("plugins_failed_probe");
    std::filesystem::create_directories(failed_probe_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(failed_probe_workspace / "runtime" / "plugin_specs" / "failed_probe_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "failed_probe_plugin" << '\t'
            << "Plugin health probe failure fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000" << '\t'
            << R"({"type":"object"})" << '\t'
            << R"({"type":"object"})" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "true" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << failing_health_args_template << '\t'
            << "1000"
            << '\n';
    }
    const auto failed_probe = RunAgentos(failed_probe_workspace, {"plugins", "health"});
    Expect(failed_probe.exit_code != 0, "plugins health should fail when a declared health probe fails");
    Expect(failed_probe.output.find("failed_probe_plugin protocol=stdio-json-v0") != std::string::npos,
        "plugins health should list plugins with failed health probes");
    Expect(failed_probe.output.find("health probe failed") != std::string::npos,
        "plugins health should explain failed health probes");

    const auto invalid_workspace = FreshWorkspace("plugins_invalid");
    std::filesystem::create_directories(invalid_workspace / "runtime" / "plugin_specs");
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "plugin_specs" / "invalid_plugin.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "invalid_plugin" << '\t'
            << "Invalid plugin protocol fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "json-rpc-v9" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto invalid = RunAgentos(invalid_workspace, {"plugins", "health"});
    Expect(invalid.exit_code != 0, "plugins health should fail when plugin specs are skipped as invalid");
    Expect(invalid.output.find("skipped_plugin") != std::string::npos,
        "plugins health should print diagnostics for skipped plugin specs");
    Expect(invalid.output.find("unsupported plugin protocol") != std::string::npos,
        "plugins health should explain skipped plugin specs");

    const auto invalid_validate = RunAgentos(invalid_workspace, {"plugins", "validate"});
    Expect(invalid_validate.exit_code != 0, "plugins validate should fail when plugin specs are invalid");
    Expect(invalid_validate.output.find("skipped_plugin") != std::string::npos,
        "plugins validate should print diagnostics for skipped plugin specs");

    const auto invalid_audit = ReadTextFile(invalid_workspace / "runtime" / "audit.log");
    Expect(invalid_audit.find(R"("event":"config_diagnostic")") != std::string::npos,
        "startup should audit skipped plugin spec diagnostics");
    Expect(invalid_audit.find(R"("source":"plugin_spec")") != std::string::npos,
        "startup plugin diagnostic audit should include the source");
    Expect(invalid_audit.find("unsupported plugin protocol") != std::string::npos,
        "startup plugin diagnostic audit should include the skip reason");

    const auto sessions_workspace = FreshWorkspace("plugins_sessions");
    const auto sessions = RunAgentos(sessions_workspace, {"plugins", "sessions"});
    Expect(sessions.exit_code == 0, "plugins sessions should exit zero with no live sessions");
    Expect(sessions.output.find("plugin_sessions_summary total=0 active=0") != std::string::npos,
        "plugins sessions should print an empty session summary on a fresh workspace");
    Expect(sessions.output.find("idle_expired=0 dead=0") != std::string::npos,
        "plugins sessions should report empty idle/dead diagnostics on a fresh workspace");
    Expect(sessions.output.find("scope=process persistence=none") != std::string::npos,
        "plugins sessions should make the per-process in-memory scope explicit");

    const auto daemon_scope_sessions = RunAgentos(sessions_workspace, {"plugins", "sessions", "scope=daemon"});
    Expect(daemon_scope_sessions.exit_code == 2,
        "plugins sessions scope=daemon should fail until cross-process session admin exists");
    Expect(daemon_scope_sessions.output.find("plugin_sessions_unavailable scope=daemon supported_scope=process") != std::string::npos,
        "plugins sessions scope=daemon should report the supported session admin scope");
    Expect(daemon_scope_sessions.output.find("cross-process plugin session admin not implemented") != std::string::npos,
        "plugins sessions scope=daemon should explain the daemon boundary");

    const auto session_close_missing = RunAgentos(sessions_workspace, {"plugins", "session-close"});
    Expect(session_close_missing.exit_code == 2,
        "plugins session-close without a name should exit with usage code 2");
    Expect(session_close_missing.output.find("missing required plugin name") != std::string::npos,
        "plugins session-close should explain when name is missing");

    const auto session_restart_missing = RunAgentos(sessions_workspace, {"plugins", "session-restart"});
    Expect(session_restart_missing.exit_code == 2,
        "plugins session-restart without a name should exit with usage code 2");
    Expect(session_restart_missing.output.find("missing required plugin name") != std::string::npos,
        "plugins session-restart should explain when name is missing");

    const auto session_close_unknown = RunAgentos(sessions_workspace, {"plugins", "session-close", "name=does_not_exist"});
    Expect(session_close_unknown.exit_code == 0,
        "plugins session-close should exit zero when there is nothing to close");
    Expect(session_close_unknown.output.find("plugin_session_close name=does_not_exist closed=0 matched=false") != std::string::npos,
        "plugins session-close should report zero closes and matched=false for an unknown plugin");

    const auto session_restart_unknown = RunAgentos(sessions_workspace, {"plugins", "session-restart", "name=does_not_exist"});
    Expect(session_restart_unknown.exit_code == 0,
        "plugins session-restart should exit zero when there are no live sessions");
    Expect(session_restart_unknown.output.find("plugin_session_restart name=does_not_exist restarted=0 matched=false") != std::string::npos,
        "plugins session-restart should report zero restarts and matched=false for an unknown plugin");

    const auto session_prune_empty = RunAgentos(sessions_workspace, {"plugins", "session-prune"});
    Expect(session_prune_empty.exit_code == 0,
        "plugins session-prune should exit zero with no live sessions");
    Expect(session_prune_empty.output.find("plugin_session_prune pruned=0 matched=false") != std::string::npos,
        "plugins session-prune should report a scriptable no-op summary on a fresh workspace");
    Expect(session_prune_empty.output.find("scope=process persistence=none") != std::string::npos,
        "plugins session-prune should make the per-process in-memory scope explicit");

    const auto session_prune_dry_run_empty = RunAgentos(sessions_workspace, {"plugins", "session-prune", "dry_run=true"});
    Expect(session_prune_dry_run_empty.exit_code == 0,
        "plugins session-prune dry_run=true should exit zero with no live sessions");
    Expect(session_prune_dry_run_empty.output.find("pruned=0 matched=false dry_run=true would_prune=0") != std::string::npos,
        "plugins session-prune dry_run=true should report a no-op preview on a fresh workspace");

    const auto daemon_scope_prune = RunAgentos(sessions_workspace, {"plugins", "session-prune", "scope=daemon"});
    Expect(daemon_scope_prune.exit_code == 2,
        "plugins session-prune scope=daemon should fail until cross-process session admin exists");
    Expect(daemon_scope_prune.output.find("plugin_sessions_unavailable scope=daemon supported_scope=process") != std::string::npos,
        "plugins session-prune scope=daemon should report the supported session admin scope");
}

void TestCliSpecsCommand() {
    const auto workspace = FreshWorkspace("cli_specs");
    std::filesystem::create_directories(workspace / "runtime" / "cli_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "cli_specs" / "valid_cli.tsv", std::ios::binary);
        spec_file
            << "valid_cli" << '\t'
            << "Valid external CLI fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto valid = RunAgentos(workspace, {"cli-specs", "validate"});
    Expect(valid.exit_code == 0, "cli-specs validate should succeed when every external CLI spec is valid");
    Expect(valid.output.find("valid_cli parse_mode=text") != std::string::npos,
        "cli-specs validate should list valid external CLI specs");
    Expect(valid.output.find("valid=true") != std::string::npos,
        "cli-specs validate should report valid specs");

    const auto invalid_workspace = FreshWorkspace("cli_specs_invalid");
    std::filesystem::create_directories(invalid_workspace / "runtime" / "cli_specs");
    {
        std::ofstream spec_file(invalid_workspace / "runtime" / "cli_specs" / "invalid_cli.tsv", std::ios::binary);
        spec_file << "too" << '\t' << "few" << '\n';
    }

    const auto invalid = RunAgentos(invalid_workspace, {"cli-specs", "validate"});
    Expect(invalid.exit_code != 0, "cli-specs validate should fail when an external CLI spec is invalid");
    Expect(invalid.output.find("skipped_cli_spec") != std::string::npos,
        "cli-specs validate should print diagnostics for skipped external CLI specs");
    Expect(invalid.output.find("CLI spec requires at least 9 fields") != std::string::npos,
        "cli-specs validate should explain skipped external CLI specs");

    const auto invalid_audit = ReadTextFile(invalid_workspace / "runtime" / "audit.log");
    Expect(invalid_audit.find(R"("event":"config_diagnostic")") != std::string::npos,
        "startup should audit skipped external CLI spec diagnostics");
    Expect(invalid_audit.find(R"("source":"cli_spec")") != std::string::npos,
        "startup CLI spec diagnostic audit should include the source");
    Expect(invalid_audit.find("CLI spec requires at least 9 fields") != std::string::npos,
        "startup CLI spec diagnostic audit should include the skip reason");
}

void TestSpecNameConflictsAreAudited() {
    const auto workspace = FreshWorkspace("spec_name_conflicts");
    std::filesystem::create_directories(workspace / "runtime" / "cli_specs");
    std::filesystem::create_directories(workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "cli_specs" / "file_read_conflict.tsv", std::ios::binary);
        spec_file
            << "file_read" << '\t'
            << "Conflicts with builtin file_read." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "file_write_conflict.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "file_write" << '\t'
            << "Conflicts with builtin file_write." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto result = RunAgentos(workspace, {"agents"});
    Expect(result.exit_code == 0, "agents command should still succeed when external specs conflict with builtin skills");

    const auto cli_validate = RunAgentos(workspace, {"cli-specs", "validate"});
    Expect(cli_validate.exit_code != 0,
        "cli-specs validate should fail when external CLI specs conflict with registered skills");
    Expect(cli_validate.output.find("external CLI spec name conflicts with already registered skill: file_read") != std::string::npos,
        "cli-specs validate should explain conflicts with registered skills");
    Expect(cli_validate.output.find("conflicting_cli_spec") != std::string::npos,
        "cli-specs validate should print source diagnostics for conflicts with registered skills");
    Expect(cli_validate.output.find("file_read_conflict.tsv") != std::string::npos,
        "cli-specs validate conflict diagnostics should include source file names");

    const auto plugin_validate = RunAgentos(workspace, {"plugins", "validate"});
    Expect(plugin_validate.exit_code != 0,
        "plugins validate should fail when plugin specs conflict with registered skills");
    Expect(plugin_validate.output.find("plugin spec name conflicts with already registered skill: file_write") != std::string::npos,
        "plugins validate should explain conflicts with registered skills");
    Expect(plugin_validate.output.find("conflicting_plugin") != std::string::npos,
        "plugins validate should print source diagnostics for conflicts with registered skills");
    Expect(plugin_validate.output.find("file_write_conflict.tsv") != std::string::npos,
        "plugins validate conflict diagnostics should include source file names");

    const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
    Expect(audit.find("external CLI spec name conflicts with already registered skill: file_read") != std::string::npos,
        "startup should audit external CLI spec conflicts with builtin skills");
    Expect(audit.find("plugin spec name conflicts with already registered skill: file_write") != std::string::npos,
        "startup should audit plugin spec conflicts with builtin skills");
}

void TestPluginNameConflictsWithExternalCliSpec() {
    const auto workspace = FreshWorkspace("plugin_cli_name_conflicts");
    std::filesystem::create_directories(workspace / "runtime" / "cli_specs");
    std::filesystem::create_directories(workspace / "runtime" / "plugin_specs");

#ifdef _WIN32
    const auto binary = "cmd";
    const auto args_template = "/d,/s,/c,echo {{message}}";
#else
    const auto binary = "sh";
    const auto args_template = "-c,printf '%s\\n' \"{{message}}\"";
#endif

    {
        std::ofstream spec_file(workspace / "runtime" / "cli_specs" / "shared_tool.tsv", std::ios::binary);
        spec_file
            << "shared_tool" << '\t'
            << "Valid external CLI fixture." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "text" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }
    {
        std::ofstream spec_file(workspace / "runtime" / "plugin_specs" / "shared_tool.tsv", std::ios::binary);
        spec_file
            << "plugin.v1" << '\t'
            << "shared_tool" << '\t'
            << "Conflicts with external CLI shared_tool." << '\t'
            << binary << '\t'
            << args_template << '\t'
            << "message" << '\t'
            << "stdio-json-v0" << '\t'
            << "low" << '\t'
            << "process.spawn" << '\t'
            << "3000"
            << '\n';
    }

    const auto cli_validate = RunAgentos(workspace, {"cli-specs", "validate"});
    Expect(cli_validate.exit_code == 0,
        "cli-specs validate should succeed when the external CLI spec itself is valid");

    const auto plugin_validate = RunAgentos(workspace, {"plugins", "validate"});
    Expect(plugin_validate.exit_code != 0,
        "plugins validate should fail when plugin specs conflict with valid external CLI specs");
    Expect(plugin_validate.output.find("plugin spec name conflicts with already registered skill: shared_tool") != std::string::npos,
        "plugins validate should explain conflicts with external CLI specs");
    Expect(plugin_validate.output.find("conflicting_plugin") != std::string::npos,
        "plugins validate should print source diagnostics for plugin/external CLI conflicts");

    const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
    Expect(audit.find("plugin spec name conflicts with already registered skill: shared_tool") != std::string::npos,
        "startup should audit plugin spec conflicts with external CLI specs");
}

void TestMemoryAndStorageCommands() {
    const auto workspace = FreshWorkspace("memory_storage");

    const auto memory = RunAgentos(workspace, {"memory", "summary"});
    Expect(memory.exit_code == 0, "memory summary command should succeed");
    Expect(memory.output.find("tasks=0") != std::string::npos, "memory summary should start with zero tasks in a fresh workspace");

    // Phase 2.2: workflow promotion now requires the same step signature to
    // recur >= 3 times AND own >= 60% of successful runs. Three identical
    // write_file invocations satisfy both gates and produce a `write_file`
    // candidate that promote-workflow can canonicalize.
    const auto first_write = RunAgentos(workspace, {
        "run", "write_file", "path=memory_storage/workflow_a.txt", "content=a", "idempotency_key=workflow-cli-a"});
    Expect(first_write.exit_code == 0, "first workflow candidate write should succeed");
    const auto second_write = RunAgentos(workspace, {
        "run", "write_file", "path=memory_storage/workflow_b.txt", "content=b", "idempotency_key=workflow-cli-b"});
    Expect(second_write.exit_code == 0, "second workflow candidate write should succeed");
    const auto third_write = RunAgentos(workspace, {
        "run", "write_file", "path=memory_storage/workflow_c.txt", "content=c", "idempotency_key=workflow-cli-c"});
    Expect(third_write.exit_code == 0, "third workflow candidate write should succeed");
    const auto promote = RunAgentos(workspace, {
        "memory", "promote-workflow", "write_file_workflow", "input_regex=branch=release/.*"});
    Expect(promote.exit_code == 0, "memory promote-workflow should accept input_regex conditions");
    Expect(promote.output.find("input_regex=branch=release/.*") != std::string::npos,
        "promote-workflow output should include input_regex conditions");

    const auto show_workflow = RunAgentos(workspace, {"memory", "show-workflow", "write_file_workflow"});
    Expect(show_workflow.exit_code == 0, "memory show-workflow should succeed for existing workflows");
    Expect(show_workflow.output.find("write_file_workflow") != std::string::npos,
        "memory show-workflow should print the selected workflow name");
    Expect(show_workflow.output.find("steps=file_write") != std::string::npos,
        "memory show-workflow should print ordered workflow steps");
    Expect(show_workflow.output.find("input_regex=branch=release/.*") != std::string::npos,
        "memory show-workflow should print workflow applicability conditions");

    const auto show_missing_workflow = RunAgentos(workspace, {"memory", "show-workflow", "missing_workflow"});
    Expect(show_missing_workflow.exit_code != 0, "memory show-workflow should fail for missing workflows");
    Expect(show_missing_workflow.output.find("workflow not found: missing_workflow") != std::string::npos,
        "memory show-workflow should explain missing workflow failures");

    const auto stored_enabled_workflows = RunAgentos(workspace, {
        "memory", "stored-workflows", "enabled=true", "trigger_task_type=write_file", "source=promoted_candidate"});
    Expect(stored_enabled_workflows.exit_code == 0,
        "memory stored-workflows should accept enabled/trigger/source filters");
    Expect(stored_enabled_workflows.output.find("write_file_workflow") != std::string::npos,
        "memory stored-workflows should include matching filtered workflows");

    const auto stored_name_filtered_workflows = RunAgentos(workspace, {
        "memory", "stored-workflows", "name_contains=missing"});
    Expect(stored_name_filtered_workflows.exit_code == 0,
        "memory stored-workflows should accept name_contains filters");
    Expect(stored_name_filtered_workflows.output.find("write_file_workflow") == std::string::npos,
        "memory stored-workflows should omit non-matching filtered workflows");

    const auto invalid_stored_filter = RunAgentos(workspace, {
        "memory", "stored-workflows", "enabled=maybe"});
    Expect(invalid_stored_filter.exit_code != 0,
        "memory stored-workflows should reject invalid enabled filter values");
    Expect(invalid_stored_filter.output.find("enabled must be true or false") != std::string::npos,
        "memory stored-workflows should explain invalid enabled filter values");

    const auto disable_workflow = RunAgentos(workspace, {
        "memory", "set-workflow-enabled", "write_file_workflow", "enabled=false"});
    Expect(disable_workflow.exit_code == 0, "memory set-workflow-enabled should disable existing workflows");
    Expect(disable_workflow.output.find("enabled=false") != std::string::npos,
        "memory set-workflow-enabled should print the updated disabled workflow");

    const auto enable_workflow = RunAgentos(workspace, {
        "memory", "set-workflow-enabled", "write_file_workflow", "enabled=true"});
    Expect(enable_workflow.exit_code == 0, "memory set-workflow-enabled should enable existing workflows");
    Expect(enable_workflow.output.find("enabled=true") != std::string::npos,
        "memory set-workflow-enabled should print the updated enabled workflow");

    const auto invalid_enable_workflow = RunAgentos(workspace, {
        "memory", "set-workflow-enabled", "write_file_workflow", "enabled=maybe"});
    Expect(invalid_enable_workflow.exit_code != 0,
        "memory set-workflow-enabled should reject invalid enabled values");
    Expect(invalid_enable_workflow.output.find("enabled must be true or false") != std::string::npos,
        "memory set-workflow-enabled should explain invalid enabled values");

    const auto validate_workflows = RunAgentos(workspace, {"memory", "validate-workflows"});
    Expect(validate_workflows.exit_code == 0, "memory validate-workflows should pass for valid workflow definitions");
    Expect(validate_workflows.output.find("valid=true") != std::string::npos,
        "memory validate-workflows should report valid workflow definitions");

    const auto explain_matching = RunAgentos(workspace, {
        "memory", "explain-workflow", "write_file_workflow", "task_type=write_file", "branch=release/2026.04"});
    Expect(explain_matching.exit_code == 0, "memory explain-workflow should succeed for existing workflows");
    Expect(explain_matching.output.find("applicable=true") != std::string::npos,
        "memory explain-workflow should report applicable=true for matching inputs");
    Expect(explain_matching.output.find("field=input_regex") != std::string::npos,
        "memory explain-workflow should include condition-level checks");

    const auto explain_non_matching = RunAgentos(workspace, {
        "memory", "explain-workflow", "write_file_workflow", "task_type=write_file", "branch=feature/demo"});
    Expect(explain_non_matching.exit_code == 0, "memory explain-workflow should not fail just because inputs do not match");
    Expect(explain_non_matching.output.find("applicable=false") != std::string::npos,
        "memory explain-workflow should report applicable=false for non-matching inputs");
    Expect(explain_non_matching.output.find("matched=false") != std::string::npos,
        "memory explain-workflow should explain which condition failed");

    const auto update_workflow = RunAgentos(workspace, {
        "memory", "update-workflow", "write_file_workflow",
        "required_inputs=path,content",
        "input_regex=branch=hotfix/.*",
        "input_bool=approved=true"});
    Expect(update_workflow.exit_code == 0, "memory update-workflow should update existing workflows");
    Expect(update_workflow.output.find("required_inputs=path,content") != std::string::npos,
        "memory update-workflow should print updated required inputs");
    Expect(update_workflow.output.find("input_regex=branch=hotfix/.*") != std::string::npos,
        "memory update-workflow should print updated regex conditions");
    Expect(update_workflow.output.find("input_bool=approved=true") != std::string::npos,
        "memory update-workflow should print updated boolean conditions");

    const auto explain_updated = RunAgentos(workspace, {
        "memory", "explain-workflow", "write_file_workflow",
        "task_type=write_file", "path=memory_storage/workflow_c.txt", "content=c",
        "branch=hotfix/urgent", "approved=true"});
    Expect(explain_updated.exit_code == 0, "memory explain-workflow should succeed after update-workflow");
    Expect(explain_updated.output.find("applicable=true") != std::string::npos,
        "updated workflow conditions should be used by explain-workflow");

    const auto rename_and_clear_workflow = RunAgentos(workspace, {
        "memory", "update-workflow", "write_file_workflow",
        "new_name=renamed_write_file_workflow",
        "input_regex=",
        "input_bool="});
    Expect(rename_and_clear_workflow.exit_code == 0,
        "memory update-workflow should rename workflows and clear list-valued conditions");
    Expect(rename_and_clear_workflow.output.find("renamed_write_file_workflow") != std::string::npos,
        "memory update-workflow should print the renamed workflow");
    Expect(rename_and_clear_workflow.output.find("input_regex= input_any=") != std::string::npos,
        "memory update-workflow should print cleared regex conditions");
    Expect(rename_and_clear_workflow.output.find("input_bool= input_regex=") != std::string::npos,
        "memory update-workflow should print cleared boolean conditions");

    const auto show_old_name_after_rename = RunAgentos(workspace, {"memory", "show-workflow", "write_file_workflow"});
    Expect(show_old_name_after_rename.exit_code != 0,
        "memory update-workflow rename should remove the old workflow name");
    Expect(show_old_name_after_rename.output.find("workflow not found: write_file_workflow") != std::string::npos,
        "show-workflow should explain old workflow name lookup failures after rename");

    const auto explain_renamed = RunAgentos(workspace, {
        "memory", "explain-workflow", "renamed_write_file_workflow",
        "task_type=write_file", "path=memory_storage/workflow_c.txt", "content=c"});
    Expect(explain_renamed.exit_code == 0, "memory explain-workflow should succeed for renamed workflows");
    Expect(explain_renamed.output.find("applicable=true") != std::string::npos,
        "cleared workflow conditions should no longer be required for applicability");

    const auto clone_workflow = RunAgentos(workspace, {
        "memory", "clone-workflow", "renamed_write_file_workflow", "new_name=cloned_write_file_workflow"});
    Expect(clone_workflow.exit_code == 0, "memory clone-workflow should clone existing workflows");
    Expect(clone_workflow.output.find("cloned_write_file_workflow") != std::string::npos,
        "memory clone-workflow should print the cloned workflow");
    Expect(clone_workflow.output.find("source=cloned_workflow") != std::string::npos,
        "memory clone-workflow should mark cloned workflow source");

    const auto show_cloned_workflow = RunAgentos(workspace, {"memory", "show-workflow", "cloned_write_file_workflow"});
    Expect(show_cloned_workflow.exit_code == 0, "memory show-workflow should find cloned workflows");
    Expect(show_cloned_workflow.output.find("steps=file_write") != std::string::npos,
        "cloned workflow should preserve ordered steps");

    const auto clone_existing_workflow = RunAgentos(workspace, {
        "memory", "clone-workflow", "renamed_write_file_workflow", "new_name=cloned_write_file_workflow"});
    Expect(clone_existing_workflow.exit_code != 0, "memory clone-workflow should reject existing target names");
    Expect(clone_existing_workflow.output.find("workflow already exists: cloned_write_file_workflow") !=
               std::string::npos,
        "memory clone-workflow should explain duplicate target failures");

    const auto invalid_update = RunAgentos(workspace, {
        "memory", "update-workflow", "renamed_write_file_workflow", "input_expr=equals:mode=workflow && ("});
    Expect(invalid_update.exit_code != 0, "memory update-workflow should reject invalid workflow definitions");
    Expect(invalid_update.output.find("field=input_expr") != std::string::npos,
        "invalid update-workflow output should name invalid fields");

    const auto invalid_promote = RunAgentos(workspace, {
        "memory", "promote-workflow", "write_file_workflow", "input_expr=equals:mode=workflow && ("});
    Expect(invalid_promote.exit_code != 0, "memory promote-workflow should reject invalid input_expr conditions");
    Expect(invalid_promote.output.find("field=input_expr") != std::string::npos,
        "invalid promote-workflow output should name the invalid input_expr field");

    {
        std::ofstream workflow_file(workspace / "runtime" / "memory" / "workflows.tsv", std::ios::app | std::ios::binary);
        workflow_file
            << "bad_workflow" << '\t'
            << "1" << '\t'
            << "write_file" << '\t'
            << "file_write" << '\t'
            << "manual" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "0" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "" << '\t'
            << "equals:mode=workflow&&("
            << '\n';
    }

    const auto invalid_validate = RunAgentos(workspace, {"memory", "validate-workflows"});
    Expect(invalid_validate.exit_code != 0, "memory validate-workflows should fail for invalid persisted workflow definitions");
    Expect(invalid_validate.output.find("workflow=bad_workflow") != std::string::npos,
        "memory validate-workflows should identify invalid workflow definitions");
    Expect(invalid_validate.output.find("field=input_expr") != std::string::npos,
        "memory validate-workflows should identify invalid workflow expression fields");

    const auto remove_cloned_workflow = RunAgentos(workspace, {"memory", "remove-workflow", "cloned_write_file_workflow"});
    Expect(remove_cloned_workflow.exit_code == 0, "memory remove-workflow should remove cloned workflows");
    Expect(remove_cloned_workflow.output.find("removed cloned_write_file_workflow") != std::string::npos,
        "memory remove-workflow should report removed cloned workflows");

    const auto remove_workflow = RunAgentos(workspace, {"memory", "remove-workflow", "renamed_write_file_workflow"});
    Expect(remove_workflow.exit_code == 0, "memory remove-workflow should remove existing workflows");
    Expect(remove_workflow.output.find("removed renamed_write_file_workflow") != std::string::npos,
        "memory remove-workflow should report removed workflows");

    const auto show_removed_workflow = RunAgentos(workspace, {"memory", "show-workflow", "renamed_write_file_workflow"});
    Expect(show_removed_workflow.exit_code != 0, "removed workflows should no longer be inspectable");
    Expect(show_removed_workflow.output.find("workflow not found: renamed_write_file_workflow") != std::string::npos,
        "show-workflow should explain removed workflow lookup failures");

    const auto remove_missing_workflow = RunAgentos(workspace, {"memory", "remove-workflow", "renamed_write_file_workflow"});
    Expect(remove_missing_workflow.exit_code != 0, "memory remove-workflow should fail for missing workflows");
    Expect(remove_missing_workflow.output.find("not_found renamed_write_file_workflow") != std::string::npos,
        "memory remove-workflow should report missing workflows");

    const auto storage = RunAgentos(workspace, {"storage", "status"});
    Expect(storage.exit_code == 0, "storage status command should succeed");
    Expect(storage.output.find("decision_id=ADR-STORAGE-001") != std::string::npos,
        "storage status should print the storage decision record id");
    Expect(storage.output.find("backend=tsv") != std::string::npos, "storage status should report TSV backend");
    Expect(storage.output.find("target_backend=sqlite") != std::string::npos,
        "storage status should report the deferred target backend");
    Expect(storage.output.find("migration_boundary=") != std::string::npos,
        "storage status should report the SQLite migration boundary");
    Expect(storage.output.find("required_interface=") != std::string::npos,
        "storage status should report required backend interface capabilities");
    Expect(storage.output.find("runtime/storage_manifest.tsv") != std::string::npos,
        "storage status should list runtime storage manifest");
    Expect(storage.output.find("runtime format=manifest.tsv version=1 path=runtime/storage_manifest.tsv exists=true") != std::string::npos,
        "storage status should report manifest existence");
    Expect(storage.output.find("bytes=") != std::string::npos && storage.output.find("lines=") != std::string::npos,
        "storage status should report file size and line counts");

    const auto storage_verify = RunAgentos(workspace, {"storage", "verify"});
    Expect(storage_verify.exit_code == 0, "storage verify should succeed in diagnostic mode");
    Expect(storage_verify.output.find("storage_verify component=runtime path=runtime/storage_manifest.tsv exists=true regular=true") != std::string::npos,
        "storage verify should report per-component file existence");
    Expect(storage_verify.output.find("storage_verify_summary") != std::string::npos,
        "storage verify should print a summary");

    const auto strict_workspace = FreshWorkspace("storage_strict_verify");
    const auto strict_status = RunAgentos(strict_workspace, {"storage", "status"});
    Expect(strict_status.exit_code == 0, "storage status should initialize a fresh storage manifest");
    const auto strict_verify = RunAgentos(strict_workspace, {"storage", "verify", "strict=true"});
    Expect(strict_verify.exit_code != 0, "storage verify strict=true should fail when manifested files are missing");
    Expect(strict_verify.output.find("strict=true") != std::string::npos &&
            strict_verify.output.find("ok=false") != std::string::npos,
        "storage verify strict=true should explain failed completeness checks");

    const auto backup_source = workspace / "storage_verify_source";
    std::filesystem::create_directories(backup_source / "runtime");
    {
        std::ofstream manifest(backup_source / "runtime" / "storage_manifest.tsv", std::ios::binary);
        manifest
            << "runtime\tmanifest.tsv\t1\truntime/storage_manifest.tsv\n"
            << "auth_sessions\ttsv\t1\truntime/auth_sessions.tsv\n";
    }
    {
        std::ofstream sessions(backup_source / "runtime" / "auth_sessions.tsv", std::ios::binary);
        sessions << "session\tdata\n";
    }
    const auto backup_verify = RunAgentos(workspace, {
        "storage", "verify", "src=" + backup_source.string(), "strict=true"});
    Expect(backup_verify.exit_code == 0, "storage verify src=... strict=true should validate complete backup sources");
    Expect(backup_verify.output.find("root=\"" + backup_source.string() + "\"") != std::string::npos,
        "storage verify src=... should report the verified root");
    std::filesystem::remove(backup_source / "runtime" / "auth_sessions.tsv");
    const auto incomplete_backup_verify = RunAgentos(workspace, {
        "storage", "verify", "src=" + backup_source.string(), "strict=true"});
    Expect(incomplete_backup_verify.exit_code != 0,
        "storage verify src=... strict=true should fail when backup source files are missing");
    Expect(incomplete_backup_verify.output.find("missing=1") != std::string::npos,
        "storage verify src=... should report missing backup source files");

    std::filesystem::create_directory(backup_source / "runtime" / "auth_sessions.tsv");
    const auto non_regular_backup_verify = RunAgentos(workspace, {
        "storage", "verify", "src=" + backup_source.string(), "strict=true"});
    Expect(non_regular_backup_verify.exit_code != 0,
        "storage verify src=... strict=true should fail when a manifested path is not a regular file");
    Expect(non_regular_backup_verify.output.find("non_regular=1") != std::string::npos,
        "storage verify src=... should report non-regular manifest paths through backend diagnostics");
    std::filesystem::remove_all(backup_source / "runtime" / "auth_sessions.tsv");

    const auto import_source = workspace / "storage_import_source";
    std::filesystem::create_directories(import_source / "runtime");
    {
        std::ofstream manifest(import_source / "runtime" / "storage_manifest.tsv", std::ios::binary);
        manifest
            << "runtime\tmanifest.tsv\t1\truntime/storage_manifest.tsv\n"
            << "auth_sessions\ttsv\t1\truntime/auth_sessions.tsv\n";
    }
    {
        std::ofstream imported(import_source / "runtime" / "auth_sessions.tsv", std::ios::binary);
        imported << "imported-session\n";
    }
    {
        std::ofstream existing(workspace / "runtime" / "auth_sessions.tsv", std::ios::binary);
        existing << "pre-import-session\n";
    }
    const auto import_result = RunAgentos(workspace, {"storage", "import", "src=" + import_source.string()});
    Expect(import_result.exit_code == 0, "storage import should succeed with a valid manifest source");
    Expect(import_result.output.find("backed_up_files=") != std::string::npos &&
            import_result.output.find("backup=") != std::string::npos,
        "storage import should report backup metadata for overwritten managed files");
    Expect(ReadTextFile(workspace / "runtime" / "auth_sessions.tsv") == "imported-session\n",
        "storage import should overwrite managed files from the source");
    const auto storage_backups = RunAgentos(workspace, {"storage", "backups"});
    Expect(storage_backups.exit_code == 0, "storage backups should succeed after import backup creation");
    Expect(storage_backups.output.find("storage_backup name=import-") != std::string::npos,
        "storage backups should list import backup directories");
    Expect(storage_backups.output.find("path=runtime/.import_backups/import-") != std::string::npos,
        "storage backups should report scriptable relative backup paths");
    Expect(storage_backups.output.find("files=") != std::string::npos &&
            storage_backups.output.find("bytes=") != std::string::npos,
        "storage backups should report backup file and byte counts");
    Expect(storage_backups.output.find("storage_backups_summary count=") != std::string::npos,
        "storage backups should print an aggregate summary");
    const auto backup_name = ExtractTokenValue(storage_backups.output, "storage_backup name=");
    Expect(!backup_name.empty(), "storage backups output should expose a reusable backup name");
    {
        std::ofstream changed(workspace / "runtime" / "auth_sessions.tsv", std::ios::binary);
        changed << "post-import-change\n";
    }
    const auto restore_backup = RunAgentos(workspace, {"storage", "restore-backup", "name=" + backup_name});
    Expect(restore_backup.exit_code == 0, "storage restore-backup should restore a listed import backup");
    Expect(restore_backup.output.find("restored_files=") != std::string::npos &&
            restore_backup.output.find("backup=" + backup_name) != std::string::npos,
        "storage restore-backup should report restored files and source backup");
    Expect(ReadTextFile(workspace / "runtime" / "auth_sessions.tsv") == "pre-import-session\n",
        "storage restore-backup should restore backed up managed files");

    const auto missing_restore = RunAgentos(workspace, {"storage", "restore-backup", "name=missing-backup"});
    Expect(missing_restore.exit_code != 0, "storage restore-backup should fail for missing backups");
    Expect(missing_restore.output.find("backup not found: missing-backup") != std::string::npos,
        "storage restore-backup should explain missing backup lookup failures");

    const auto unsafe_restore = RunAgentos(workspace, {"storage", "restore-backup", "name=..\\outside"});
    Expect(unsafe_restore.exit_code != 0, "storage restore-backup should reject path-like backup names");
    Expect(unsafe_restore.output.find("name must be a backup directory name") != std::string::npos,
        "storage restore-backup should explain invalid backup names");

    const auto empty_backups_workspace = FreshWorkspace("storage_backups_empty");
    const auto empty_backups = RunAgentos(empty_backups_workspace, {"storage", "backups"});
    Expect(empty_backups.exit_code == 0, "storage backups should succeed without any backup directories");
    Expect(empty_backups.output.find("storage_backups_summary count=0 files=0 bytes=0") != std::string::npos,
        "storage backups should report an empty summary for a fresh workspace");

    const auto committed_txn = workspace / "runtime" / ".transactions" / "cli-recover";
    std::filesystem::create_directories(committed_txn);
    {
        std::ofstream data(committed_txn / "file_0.data", std::ios::binary);
        data << "recovered-by-cli\n";
    }
    {
        std::ofstream prepare(committed_txn / "prepare.tsv", std::ios::binary);
        prepare << (workspace / "runtime" / "recovered.tsv").generic_string() << '\t' << "file_0.data\n";
    }
    {
        std::ofstream commit(committed_txn / "commit", std::ios::binary);
        commit << "committed\n";
    }
    const auto rolled_back_txn = workspace / "runtime" / ".transactions" / "cli-rollback";
    std::filesystem::create_directories(rolled_back_txn);
    {
        std::ofstream data(rolled_back_txn / "file_0.data", std::ios::binary);
        data << "should-not-apply\n";
    }
    {
        std::ofstream prepare(rolled_back_txn / "prepare.tsv", std::ios::binary);
        prepare << (workspace / "runtime" / "rolled_back.tsv").generic_string() << '\t' << "file_0.data\n";
    }
    const auto corrupt_txn = workspace / "runtime" / ".transactions" / "cli-corrupt";
    std::filesystem::create_directories(corrupt_txn);
    {
        std::ofstream prepare(corrupt_txn / "prepare.tsv", std::ios::binary);
        prepare << (workspace / "runtime" / "corrupt_recovered.tsv").generic_string() << '\t' << "missing.data\n";
    }
    {
        std::ofstream commit(corrupt_txn / "commit", std::ios::binary);
        commit << "committed\n";
    }

    const auto recover = RunAgentos(workspace, {"storage", "recover"});
    Expect(recover.exit_code == 0, "storage recover command should succeed");
    Expect(recover.output.find("committed_replayed=1") != std::string::npos,
        "storage recover should replay committed transaction markers");
    Expect(recover.output.find("rolled_back=1") != std::string::npos,
        "storage recover should roll back uncommitted prepared transactions");
    Expect(recover.output.find("failed=1") != std::string::npos,
        "storage recover should report corrupt committed transaction failures");
    Expect(ReadTextFile(workspace / "runtime" / "recovered.tsv") == "recovered-by-cli\n",
        "storage recover should apply committed transaction data");
    Expect(!std::filesystem::exists(workspace / "runtime" / "rolled_back.tsv"),
        "storage recover should not apply uncommitted transaction data");
    Expect(!std::filesystem::exists(workspace / "runtime" / "corrupt_recovered.tsv"),
        "storage recover should not apply corrupt committed transaction data");
}

void TestTrustCommands() {
    const auto workspace = FreshWorkspace("trust");

    const auto identity_add = RunAgentos(workspace, {
        "trust", "identity-add", "identity=phone", "user=local-user", "label=dev-phone"});
    Expect(identity_add.exit_code == 0, "trust identity-add should succeed");
    Expect(identity_add.output.find("phone user=local-user label=dev-phone") != std::string::npos,
        "trust identity-add should print the saved identity");

    const auto identities = RunAgentos(workspace, {"trust", "identities"});
    Expect(identities.exit_code == 0, "trust identities should succeed");
    Expect(identities.output.find("phone user=local-user label=dev-phone") != std::string::npos,
        "trust identities should list the saved identity");

    const auto pair = RunAgentos(workspace, {
        "trust", "pair", "identity=phone", "device=device1", "label=dev-phone", "permissions=task.submit"});
    Expect(pair.exit_code == 0, "trust pair should succeed");
    Expect(pair.output.find("phone device=device1") != std::string::npos,
        "trust pair should print the paired peer");

    const auto list = RunAgentos(workspace, {"trust", "list"});
    Expect(list.exit_code == 0, "trust list should succeed");
    Expect(list.output.find("permissions=task.submit") != std::string::npos,
        "trust list should include paired permissions");
    Expect(list.output.find("paired_epoch_ms=") != std::string::npos
            && list.output.find("last_seen_epoch_ms=") != std::string::npos,
        "trust list should include device lifecycle timestamps");

    const auto device_label = RunAgentos(workspace, {
        "trust", "device-label", "identity=phone", "device=device1", "label=renamed-phone"});
    Expect(device_label.exit_code == 0, "trust device-label should succeed");
    const auto device_seen = RunAgentos(workspace, {"trust", "device-seen", "identity=phone", "device=device1"});
    Expect(device_seen.exit_code == 0, "trust device-seen should succeed");
    const auto block = RunAgentos(workspace, {"trust", "block", "identity=phone", "device=device1"});
    Expect(block.exit_code == 0, "trust block should succeed");
    const auto unblock = RunAgentos(workspace, {"trust", "unblock", "identity=phone", "device=device1"});
    Expect(unblock.exit_code == 0, "trust unblock should succeed");
    const auto list_after_lifecycle = RunAgentos(workspace, {"trust", "list"});
    Expect(list_after_lifecycle.output.find("label=renamed-phone") != std::string::npos,
        "trust list should include renamed device label");
    Expect(list_after_lifecycle.output.find("trust=paired") != std::string::npos,
        "trust unblock should restore paired trust state");

    const auto device_show = RunAgentos(workspace, {"trust", "device-show", "identity=phone", "device=device1"});
    Expect(device_show.exit_code == 0, "trust device-show should succeed for existing paired devices");
    Expect(device_show.output.find("phone device=device1") != std::string::npos,
        "trust device-show should print the requested device");
    Expect(device_show.output.find("label=renamed-phone") != std::string::npos,
        "trust device-show should print device lifecycle metadata");

    const auto device_show_missing = RunAgentos(workspace, {"trust", "device-show", "identity=phone", "device=missing"});
    Expect(device_show_missing.exit_code != 0, "trust device-show should fail for missing devices");
    Expect(device_show_missing.output.find("not_found phone device=missing") != std::string::npos,
        "trust device-show should report missing devices");

    const auto invite_create = RunAgentos(workspace, {
        "trust", "invite-create", "identity=tablet", "device=device2", "label=dev-tablet",
        "user=local-user", "identity_label=tablet-identity", "permissions=task.submit", "ttl_seconds=600"});
    Expect(invite_create.exit_code == 0, "trust invite-create should succeed");
    Expect(invite_create.output.find("invite token=") != std::string::npos,
        "trust invite-create should print an invite token");
    const auto token_prefix = std::string("token=");
    const auto token_start = invite_create.output.find(token_prefix);
    std::string invite_token;
    if (token_start != std::string::npos) {
        const auto value_start = token_start + token_prefix.size();
        const auto value_end = invite_create.output.find(' ', value_start);
        invite_token = invite_create.output.substr(value_start, value_end - value_start);
    }
    Expect(!invite_token.empty(), "trust invite-create token should be parseable");

    const auto invites = RunAgentos(workspace, {"trust", "invites"});
    Expect(invites.exit_code == 0, "trust invites should succeed");
    Expect(invites.output.find(invite_token) != std::string::npos,
        "trust invites should list active invite tokens");

    const auto invite_accept = RunAgentos(workspace, {"trust", "invite-accept", "token=" + invite_token});
    Expect(invite_accept.exit_code == 0, "trust invite-accept should succeed");
    Expect(invite_accept.output.find("tablet device=device2") != std::string::npos,
        "trust invite-accept should pair the invited device");

    const auto invite_accept_again = RunAgentos(workspace, {"trust", "invite-accept", "token=" + invite_token});
    Expect(invite_accept_again.exit_code != 0, "trust invite-accept should reject consumed tokens");

    const auto role_set = RunAgentos(workspace, {
        "trust", "role-set", "role=reader", "permissions=filesystem.read"});
    Expect(role_set.exit_code == 0, "trust role-set should succeed");
    Expect(role_set.output.find("role reader permissions=filesystem.read") != std::string::npos,
        "trust role-set should print saved role permissions");

    const auto user_role = RunAgentos(workspace, {
        "trust", "user-role", "user=alice", "roles=reader"});
    Expect(user_role.exit_code == 0, "trust user-role should succeed");
    Expect(user_role.output.find("user alice roles=reader") != std::string::npos,
        "trust user-role should print saved user role assignment");

    const auto roles = RunAgentos(workspace, {"trust", "roles"});
    Expect(roles.exit_code == 0, "trust roles should succeed");
    Expect(roles.output.find("role reader permissions=filesystem.read") != std::string::npos,
        "trust roles should list persisted role definitions");
    Expect(roles.output.find("user alice roles=reader") != std::string::npos,
        "trust roles should list persisted user role assignments");

    const auto role_show = RunAgentos(workspace, {"trust", "role-show", "role=reader"});
    Expect(role_show.exit_code == 0, "trust role-show should succeed for existing roles");
    Expect(role_show.output.find("role reader permissions=filesystem.read") != std::string::npos,
        "trust role-show should print the requested role");

    const auto role_show_missing = RunAgentos(workspace, {"trust", "role-show", "role=missing"});
    Expect(role_show_missing.exit_code != 0, "trust role-show should fail for missing roles");
    Expect(role_show_missing.output.find("not_found role missing") != std::string::npos,
        "trust role-show should report missing roles");

    const auto user_role_show = RunAgentos(workspace, {"trust", "user-role-show", "user=alice"});
    Expect(user_role_show.exit_code == 0, "trust user-role-show should succeed for existing assignments");
    Expect(user_role_show.output.find("user alice roles=reader") != std::string::npos,
        "trust user-role-show should print the requested user assignment");

    const auto user_role_show_missing = RunAgentos(workspace, {"trust", "user-role-show", "user=bob"});
    Expect(user_role_show_missing.exit_code != 0, "trust user-role-show should fail for missing assignments");
    Expect(user_role_show_missing.output.find("not_found user bob") != std::string::npos,
        "trust user-role-show should report missing assignments");

    const auto approval_request = RunAgentos(workspace, {
        "trust", "approval-request", "subject=critical-write", "reason=operator-check", "requested_by=alice"});
    Expect(approval_request.exit_code == 0, "trust approval-request should succeed");
    Expect(approval_request.output.find("status=pending") != std::string::npos,
        "trust approval-request should create pending approvals");
    const auto approval_prefix = std::string("approval ");
    const auto approval_start = approval_request.output.find(approval_prefix);
    std::string approval_id;
    if (approval_start != std::string::npos) {
        const auto value_start = approval_start + approval_prefix.size();
        const auto value_end = approval_request.output.find(' ', value_start);
        approval_id = approval_request.output.substr(value_start, value_end - value_start);
    }
    Expect(!approval_id.empty(), "trust approval-request id should be parseable");

    const auto approval_approve = RunAgentos(workspace, {
        "trust", "approval-approve", "approval=" + approval_id, "approved_by=admin"});
    Expect(approval_approve.exit_code == 0, "trust approval-approve should succeed");
    Expect(approval_approve.output.find("status=approved") != std::string::npos,
        "trust approval-approve should mark approvals approved");

    const auto approval_show = RunAgentos(workspace, {"trust", "approval-show", "approval=" + approval_id});
    Expect(approval_show.exit_code == 0, "trust approval-show should succeed for existing approvals");
    Expect(approval_show.output.find("approval " + approval_id) != std::string::npos,
        "trust approval-show should print the requested approval");
    Expect(approval_show.output.find("status=approved") != std::string::npos,
        "trust approval-show should print approval status");

    const auto approval_show_missing = RunAgentos(workspace, {"trust", "approval-show", "approval=missing-approval"});
    Expect(approval_show_missing.exit_code != 0, "trust approval-show should fail for missing approvals");
    Expect(approval_show_missing.output.find("not_found approval missing-approval") != std::string::npos,
        "trust approval-show should report missing approvals");

    const auto approvals = RunAgentos(workspace, {"trust", "approvals"});
    Expect(approvals.exit_code == 0, "trust approvals should succeed");
    Expect(approvals.output.find(approval_id) != std::string::npos,
        "trust approvals should list persisted approvals");

    const auto approval_revoke = RunAgentos(workspace, {
        "trust", "approval-revoke", "approval=" + approval_id, "approved_by=admin"});
    Expect(approval_revoke.exit_code == 0, "trust approval-revoke should succeed");
    Expect(approval_revoke.output.find("status=revoked") != std::string::npos,
        "trust approval-revoke should mark approvals revoked");

    {
        std::ofstream readme(workspace / "README.md", std::ios::binary);
        readme << "role catalog fixture\n";
    }
    const auto role_allowed_read = RunAgentos(workspace, {
        "run", "read_file", "user=alice", "path=README.md"});
    Expect(role_allowed_read.exit_code == 0, "role catalog should allow user role filesystem.read");

    const auto role_denied_write = RunAgentos(workspace, {
        "run", "write_file", "user=alice", "path=runtime/role-denied.txt", "content=nope", "idempotency_key=role-denied"});
    Expect(role_denied_write.exit_code != 0, "role catalog should deny missing user role filesystem.write");
    Expect(role_denied_write.output.find("filesystem.write") != std::string::npos,
        "role catalog denial should name the missing write permission");

    const auto user_role_remove = RunAgentos(workspace, {
        "trust", "user-role-remove", "user=alice"});
    Expect(user_role_remove.exit_code == 0, "trust user-role-remove should succeed for existing assignments");
    const auto read_after_user_remove = RunAgentos(workspace, {
        "run", "read_file", "user=alice", "path=README.md"});
    Expect(read_after_user_remove.exit_code == 0,
        "removing user roles should fall back to unconstrained local task behavior");

    const auto user_role_restore = RunAgentos(workspace, {
        "trust", "user-role", "user=alice", "roles=reader"});
    Expect(user_role_restore.exit_code == 0, "trust user-role should allow restoring a user assignment");

    const auto role_remove = RunAgentos(workspace, {
        "trust", "role-remove", "role=reader"});
    Expect(role_remove.exit_code == 0, "trust role-remove should remove existing roles");
    const auto roles_after_remove = RunAgentos(workspace, {"trust", "roles"});
    Expect(roles_after_remove.output.find("role reader permissions=filesystem.read") == std::string::npos,
        "trust roles should not list removed roles");
    Expect(roles_after_remove.output.find("user alice roles=reader") == std::string::npos,
        "removing a role should clean user assignments that referenced it");
}

void TestScheduleCommands() {
    const auto workspace = FreshWorkspace("schedule");

    const auto add = RunAgentos(workspace, {
        "schedule", "add", "id=cli-schedule", "task=write_file", "due=now",
        "path=runtime/scheduled.txt", "content=hello"});
    Expect(add.exit_code == 0, "schedule add should succeed");
    Expect(add.output.find("cli-schedule enabled=true") != std::string::npos,
        "schedule add should print the stored task");

    const auto list = RunAgentos(workspace, {"schedule", "list"});
    Expect(list.exit_code == 0, "schedule list should succeed");
    Expect(list.output.find("cli-schedule") != std::string::npos, "schedule list should include the added task");

    const auto add_cron = RunAgentos(workspace, {
        "schedule", "add", "id=cli-cron", "task=write_file", "cron=* * * * *",
        "path=runtime/cron.txt", "content=cron"});
    Expect(add_cron.exit_code == 0, "schedule add should accept a five-field cron expression");
    Expect(add_cron.output.find("cron=\"* * * * *\"") != std::string::npos,
        "schedule add should print the configured cron expression");

    const auto add_cron_alias = RunAgentos(workspace, {
        "schedule", "add", "id=cli-cron-alias", "task=write_file", "cron=@hourly",
        "path=runtime/cron-alias.txt", "content=cron-alias"});
    Expect(add_cron_alias.exit_code == 0, "schedule add should accept @hourly cron alias");
    Expect(add_cron_alias.output.find("cron=\"@hourly\"") != std::string::npos,
        "schedule add should print the configured cron alias");

    const auto invalid_cron = RunAgentos(workspace, {
        "schedule", "add", "id=cli-bad-cron", "task=write_file", "cron=*/0 * * * *",
        "path=runtime/bad-cron.txt", "content=bad"});
    Expect(invalid_cron.exit_code != 0, "schedule add should reject invalid cron expressions");
    Expect(invalid_cron.output.find("cron expression must use five fields") != std::string::npos,
        "schedule add should explain invalid cron expressions");

    const auto run_due = RunAgentos(workspace, {"schedule", "run-due"});
    Expect(run_due.exit_code == 0, "schedule run-due should succeed for a valid write task");
    Expect(run_due.output.find("cli-schedule success=true") != std::string::npos,
        "schedule run-due should report successful execution");
    Expect(std::filesystem::exists(workspace / "runtime" / "scheduled.txt"),
        "schedule run-due should create the scheduled output file");

    const auto history = RunAgentos(workspace, {"schedule", "history"});
    Expect(history.exit_code == 0, "schedule history should succeed");
    Expect(history.output.find("cli-schedule") != std::string::npos,
        "schedule history should include the executed task");

    const auto remove = RunAgentos(workspace, {"schedule", "remove", "id=cli-schedule"});
    Expect(remove.exit_code == 0, "schedule remove should succeed");
    Expect(remove.output.find("removed cli-schedule") != std::string::npos,
        "schedule remove should confirm deletion");
}

void TestSubagentsCommand() {
    const auto workspace = FreshWorkspace("subagents");
    const auto result = RunAgentos(workspace, {
        "subagents", "run", "agents=local_planner", "mode=sequential", "objective=Plan_the_next_phase"});

    Expect(result.exit_code == 0, "subagents run should succeed for local_planner");
    Expect(result.output.find("success: true") != std::string::npos,
        "subagents run should print a successful result");
    Expect(result.output.find("local_planner") != std::string::npos,
        "subagents run should mention the selected agent");

    const auto roles_result = RunAgentos(workspace, {
        "subagents", "run", "agents=local_planner", "mode=sequential", "roles=local_planner:planner", "objective=Plan_with_role"});
    Expect(roles_result.exit_code == 0, "subagents run should accept explicit role assignments");
    Expect(roles_result.output.find("\"roles\":\"planner\"") != std::string::npos,
        "subagents run output should include explicit role assignment");

    const auto decomposed_result = RunAgentos(workspace, {
        "subagents", "run", "agents=local_planner", "mode=sequential", "roles=local_planner:planner",
        "auto_decompose=true", "decomposition_agent=local_planner", "objective=Coordinate_decomposed_work"});
    Expect(decomposed_result.exit_code == 0, "subagents run should support planner-generated decomposition");
    Expect(decomposed_result.output.find(R"("decomposition_agent":"local_planner")") != std::string::npos,
        "subagents run output should include the decomposition agent");
}

void TestAuthCommands() {
    const auto workspace = FreshWorkspace("auth");

    const auto providers = RunAgentos(workspace, {"auth", "providers"});
    Expect(providers.exit_code == 0, "auth providers should succeed");
    Expect(providers.output.find("openai") != std::string::npos, "auth providers should list openai");
    Expect(providers.output.find("qwen") != std::string::npos, "auth providers should list qwen");

    const auto credential_store = RunAgentos(workspace, {"auth", "credential-store"});
    Expect(credential_store.exit_code == 0, "auth credential-store should succeed");
#ifdef _WIN32
    Expect(credential_store.output.find("backend=windows-credential-manager") != std::string::npos,
        "auth credential-store should report Windows Credential Manager backend on Windows");
#else
    Expect(
        credential_store.output.find("backend=linux-secret-service") != std::string::npos ||
            credential_store.output.find("backend=env-ref-only") != std::string::npos,
        "auth credential-store should report a supported Linux credential backend");
#endif

    const auto oauth_defaults = RunAgentos(workspace, {"auth", "oauth-defaults"});
    Expect(oauth_defaults.exit_code == 0, "auth oauth-defaults should succeed when any provider has defaults");
    Expect(oauth_defaults.output.find("oauth_defaults provider=gemini") != std::string::npos,
        "auth oauth-defaults should list Gemini defaults");
    Expect(oauth_defaults.output.find("supported=true") != std::string::npos,
        "auth oauth-defaults should mark supported provider defaults");
    Expect(oauth_defaults.output.find("endpoint_status=available") != std::string::npos,
        "auth oauth-defaults should mark providers with usable endpoints as available");
    Expect(oauth_defaults.output.find("https://oauth2.googleapis.com/token") != std::string::npos,
        "auth oauth-defaults should print token endpoints");
    Expect(oauth_defaults.output.find("cloud-platform") != std::string::npos,
        "auth oauth-defaults should print default scopes");

    std::filesystem::create_directories(workspace / "runtime");
    {
        std::ofstream configured_defaults(workspace / "runtime" / "auth_oauth_providers.tsv", std::ios::binary);
        configured_defaults
            << "# provider\tauthorization_endpoint\ttoken_endpoint\tscopes\n"
            << "gemini\thttps://accounts.example.test/custom-auth\thttps://accounts.example.test/custom-token\topenid,email\n";
    }
    const auto configured_oauth_defaults = RunAgentos(workspace, {"auth", "oauth-defaults", "gemini"});
    Expect(configured_oauth_defaults.exit_code == 0, "auth oauth-defaults should accept repo-local configured defaults");
    Expect(configured_oauth_defaults.output.find("https://accounts.example.test/custom-auth") != std::string::npos,
        "auth oauth-defaults should print configured authorization endpoint");
    Expect(configured_oauth_defaults.output.find("https://accounts.example.test/custom-token") != std::string::npos,
        "auth oauth-defaults should print configured token endpoint");
    Expect(configured_oauth_defaults.output.find("scopes=\"openid,email\"") != std::string::npos,
        "auth oauth-defaults should print configured scopes");
    const auto valid_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate"});
    Expect(valid_oauth_config.exit_code == 0, "auth oauth-config-validate should accept valid repo-local defaults");
    Expect(valid_oauth_config.output.find("valid=true") != std::string::npos,
        "auth oauth-config-validate should report valid configs");

    const auto all_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate", "--all"});
    Expect(all_oauth_config.exit_code == 0, "auth oauth-config-validate --all should succeed for valid configs");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=gemini") != std::string::npos,
        "oauth-config-validate --all should enumerate gemini provider audit row");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=openai") != std::string::npos,
        "oauth-config-validate --all should enumerate openai provider audit row");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=anthropic") != std::string::npos,
        "oauth-config-validate --all should enumerate anthropic provider audit row");
    Expect(all_oauth_config.output.find("oauth_config_provider provider=qwen") != std::string::npos,
        "oauth-config-validate --all should enumerate qwen provider audit row");
    Expect(all_oauth_config.output.find("origin=stub") != std::string::npos,
        "oauth-config-validate --all should mark stubbed providers with origin=stub");
    Expect(all_oauth_config.output.find("endpoint_status=deferred") != std::string::npos,
        "oauth-config-validate --all should mark stubbed provider endpoints as deferred");
    Expect(all_oauth_config.output.find("origin=config") != std::string::npos,
        "oauth-config-validate --all should mark provider with origin=config when overridden");
    Expect(all_oauth_config.output.find("endpoint_status=available") != std::string::npos,
        "oauth-config-validate --all should mark configured complete endpoints as available");

    const auto oauth_defaults_origin = RunAgentos(workspace, {"auth", "oauth-defaults", "anthropic"});
    Expect(oauth_defaults_origin.output.find("origin=stub") != std::string::npos,
        "auth oauth-defaults should report origin=stub for unsupported providers");
    Expect(oauth_defaults_origin.output.find("endpoint_status=deferred") != std::string::npos,
        "auth oauth-defaults should report deferred endpoint status for stubbed providers");
    Expect(oauth_defaults_origin.output.find("note=") != std::string::npos,
        "auth oauth-defaults should include a note for stubbed providers");

    const auto qwen_oauth_defaults = RunAgentos(workspace, {"auth", "oauth-defaults", "qwen"});
    Expect(qwen_oauth_defaults.exit_code != 0,
        "auth oauth-defaults should return non-zero for providers without built-in OAuth defaults");
    Expect(qwen_oauth_defaults.output.find("oauth_defaults provider=qwen") != std::string::npos,
        "auth oauth-defaults should still print unsupported provider discovery data");
    Expect(qwen_oauth_defaults.output.find("supported=false") != std::string::npos,
        "auth oauth-defaults should mark unsupported provider defaults");
    Expect(qwen_oauth_defaults.output.find("endpoint_status=deferred") != std::string::npos,
        "auth oauth-defaults should mark stubbed provider endpoints as deferred");

    const auto oauth_start = RunAgentos(workspace, {
        "auth", "oauth-start", "gemini",
        "client_id=test-client",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "profile=oauth-smoke"});
    Expect(oauth_start.exit_code == 0, "auth oauth-start should succeed for browser OAuth providers");
    Expect(oauth_start.output.find("oauth_start provider=gemini profile=oauth-smoke") != std::string::npos,
        "auth oauth-start should print provider and profile");
    Expect(oauth_start.output.find("code_challenge_method=S256") != std::string::npos,
        "auth oauth-start should use PKCE S256");
    Expect(oauth_start.output.find("authorization_url=") != std::string::npos,
        "auth oauth-start should print authorization URL");
    Expect(oauth_start.output.find("https://accounts.example.test/custom-auth") != std::string::npos,
        "auth oauth-start should use repo-local configured authorization endpoint");
    Expect(oauth_start.output.find("scope=openid%20email") != std::string::npos,
        "auth oauth-start should encode configured scopes in authorization URL");
    const auto missing_oauth_start_input = RunAgentos(workspace, {
        "auth", "oauth-start", "gemini",
        "client_id=test-client"});
    Expect(missing_oauth_start_input.exit_code != 0,
        "auth oauth-start should reject missing redirect_uri before building the authorization URL");
    Expect(missing_oauth_start_input.output.find("oauth_input_error command=oauth-start provider=gemini") != std::string::npos,
        "auth oauth-start should print a structured missing-input diagnostic");
    Expect(missing_oauth_start_input.output.find("missing_fields=\"redirect_uri\"") != std::string::npos,
        "auth oauth-start should name the missing redirect_uri field");
    Expect(missing_oauth_start_input.output.find("endpoint_status=available") != std::string::npos,
        "auth oauth-start missing-input diagnostics should include endpoint status");
    const auto missing_oauth_login_input = RunAgentos(workspace, {
        "auth", "oauth-login", "gemini",
        "client_id=test-client",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "token_endpoint="});
    Expect(missing_oauth_login_input.exit_code != 0,
        "auth oauth-login should reject explicit empty token endpoints");
    Expect(missing_oauth_login_input.output.find("oauth_input_error command=oauth-login provider=gemini") != std::string::npos,
        "auth oauth-login should print a structured missing-input diagnostic");
    Expect(missing_oauth_login_input.output.find("missing_fields=\"token_endpoint\"") != std::string::npos,
        "auth oauth-login should name the missing token_endpoint field");
    Expect(missing_oauth_login_input.output.find("endpoint_status=available") != std::string::npos,
        "auth oauth-login missing-input diagnostics should include endpoint status");

    {
        std::ofstream invalid_defaults(workspace / "runtime" / "auth_oauth_providers.tsv", std::ios::binary);
        invalid_defaults << "qwen\thttps://accounts.example.test/custom-auth\t\n";
    }
    const auto invalid_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate"});
    Expect(invalid_oauth_config.exit_code != 0, "auth oauth-config-validate should reject invalid repo-local defaults");
    Expect(invalid_oauth_config.output.find("token_endpoint is required") != std::string::npos,
        "auth oauth-config-validate should explain missing token endpoints");
    const auto invalid_all_oauth_config = RunAgentos(workspace, {"auth", "oauth-config-validate", "--all"});
    Expect(invalid_all_oauth_config.exit_code != 0,
        "auth oauth-config-validate --all should reject invalid repo-local defaults");
    Expect(invalid_all_oauth_config.output.find("oauth_config_provider provider=qwen") != std::string::npos,
        "auth oauth-config-validate --all should still print the invalid provider audit row");
    Expect(invalid_all_oauth_config.output.find("origin=config") != std::string::npos,
        "auth oauth-config-validate --all should mark incomplete workspace endpoint overrides as config");
    Expect(invalid_all_oauth_config.output.find("endpoint_status=missing") != std::string::npos,
        "auth oauth-config-validate --all should mark incomplete endpoint overrides as missing");

    std::filesystem::remove(workspace / "runtime" / "auth_oauth_providers.tsv");
    const auto default_oauth_start = RunAgentos(workspace, {
        "auth", "oauth-start", "gemini",
        "client_id=test-client",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "profile=oauth-defaults"});
    Expect(default_oauth_start.exit_code == 0, "auth oauth-start should use provider defaults when endpoints are omitted");
    Expect(default_oauth_start.output.find("https://accounts.google.com/o/oauth2/v2/auth") != std::string::npos,
        "auth oauth-start should use Gemini's default authorization endpoint");
    Expect(default_oauth_start.output.find("cloud-platform") != std::string::npos,
        "auth oauth-start should include Gemini's default cloud-platform scope");

    const auto unsupported_oauth = RunAgentos(workspace, {
        "auth", "oauth-start", "qwen",
        "client_id=test-client",
        "authorization_endpoint=https://accounts.example.test/oauth",
        "redirect_uri=http://127.0.0.1:48177/callback"});
    Expect(unsupported_oauth.exit_code != 0, "auth oauth-start should reject providers without browser OAuth support");
    Expect(unsupported_oauth.output.find("provider does not support browser OAuth") != std::string::npos,
        "auth oauth-start should explain unsupported providers");

    const auto oauth_callback = RunAgentos(workspace, {
        "auth", "oauth-callback",
        "callback_url=http://127.0.0.1:48177/callback?code=auth%2Fcode&state=state-123",
        "state=state-123"});
    Expect(oauth_callback.exit_code == 0, "auth oauth-callback should succeed with required inputs");
    Expect(oauth_callback.output.find("oauth_callback success=true code=auth/code") != std::string::npos,
        "auth oauth-callback should decode and print callback code");

    const auto oauth_callback_invalid = RunAgentos(workspace, {
        "auth", "oauth-callback",
        "callback_url=http://127.0.0.1:48177/callback?code=auth-code&state=wrong",
        "state=state-123"});
    Expect(oauth_callback_invalid.exit_code == 0, "auth oauth-callback should report invalid callbacks as data");
    Expect(oauth_callback_invalid.output.find("success=false error=InvalidOAuthState") != std::string::npos,
        "auth oauth-callback should report invalid state");
    const auto missing_oauth_callback_input = RunAgentos(workspace, {"auth", "oauth-callback"});
    Expect(missing_oauth_callback_input.exit_code != 0,
        "auth oauth-callback should reject missing callback_url and state");
    Expect(missing_oauth_callback_input.output.find("oauth_input_error command=oauth-callback") != std::string::npos,
        "auth oauth-callback should print a structured missing-input diagnostic");
    Expect(missing_oauth_callback_input.output.find("missing_fields=\"callback_url,state\"") != std::string::npos,
        "auth oauth-callback should name missing callback_url and state fields");
    Expect(missing_oauth_callback_input.output.find("callback_url is required") != std::string::npos,
        "auth oauth-callback should preserve the first missing-field message");
    const auto missing_oauth_listen_input = RunAgentos(workspace, {"auth", "oauth-listen"});
    Expect(missing_oauth_listen_input.exit_code != 0,
        "auth oauth-listen should reject missing state and port");
    Expect(missing_oauth_listen_input.output.find("oauth_input_error command=oauth-listen") != std::string::npos,
        "auth oauth-listen should print a structured missing-input diagnostic");
    Expect(missing_oauth_listen_input.output.find("missing_fields=\"state,port\"") != std::string::npos,
        "auth oauth-listen should name missing state and port fields");
    Expect(missing_oauth_listen_input.output.find("state is required") != std::string::npos,
        "auth oauth-listen should preserve the first missing-field message");

    const auto token_request = RunAgentos(workspace, {
        "auth", "oauth-token-request",
        "token_endpoint=https://oauth2.example.test/token",
        "client_id=client id",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "code=code/value",
        "code_verifier=verifier"});
    Expect(token_request.exit_code == 0, "auth oauth-token-request should succeed with required inputs");
    Expect(token_request.output.find("content_type=application/x-www-form-urlencoded") != std::string::npos,
        "auth oauth-token-request should report form content type");
    Expect(token_request.output.find("grant_type=authorization_code") != std::string::npos,
        "auth oauth-token-request should include authorization_code grant");
    Expect(token_request.output.find("client_id=client%20id") != std::string::npos,
        "auth oauth-token-request should URL-encode client id");
    Expect(token_request.output.find("code=code%2Fvalue") != std::string::npos,
        "auth oauth-token-request should URL-encode authorization code");

    const auto missing_token_request = RunAgentos(workspace, {"auth", "oauth-token-request", "client_id=test"});
    Expect(missing_token_request.exit_code != 0, "auth oauth-token-request should reject missing required inputs");
    Expect(missing_token_request.output.find("oauth_input_error command=oauth-token-request") != std::string::npos,
        "auth oauth-token-request should print a structured missing-input diagnostic");
    Expect(missing_token_request.output.find("missing_fields=\"token_endpoint,redirect_uri,code,code_verifier\"") != std::string::npos,
        "auth oauth-token-request should name every missing token request field");
    Expect(missing_token_request.output.find("token_endpoint is required") != std::string::npos,
        "auth oauth-token-request should explain missing token endpoint");

    const auto refresh_request = RunAgentos(workspace, {
        "auth", "oauth-refresh-request",
        "token_endpoint=https://oauth2.example.test/token",
        "client_id=client id",
        "refresh_token=refresh/value"});
    Expect(refresh_request.exit_code == 0, "auth oauth-refresh-request should succeed with required inputs");
    Expect(refresh_request.output.find("grant_type=refresh_token") != std::string::npos,
        "auth oauth-refresh-request should include refresh_token grant");
    Expect(refresh_request.output.find("refresh_token=refresh%2Fvalue") != std::string::npos,
        "auth oauth-refresh-request should URL-encode refresh token");

    const auto missing_refresh_request = RunAgentos(workspace, {"auth", "oauth-refresh-request", "client_id=test"});
    Expect(missing_refresh_request.exit_code != 0, "auth oauth-refresh-request should reject missing required inputs");
    Expect(missing_refresh_request.output.find("oauth_input_error command=oauth-refresh-request") != std::string::npos,
        "auth oauth-refresh-request should print a structured missing-input diagnostic");
    Expect(missing_refresh_request.output.find("missing_fields=\"token_endpoint,refresh_token\"") != std::string::npos,
        "auth oauth-refresh-request should name every missing refresh request field");
    Expect(missing_refresh_request.output.find("token_endpoint is required") != std::string::npos,
        "auth oauth-refresh-request should explain missing token endpoint");

    const auto missing_oauth_complete_input = RunAgentos(workspace, {
        "auth", "oauth-complete", "gemini",
        "state=state-123",
        "code_verifier=verifier",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "client_id=client-id",
        "token_endpoint="});
    Expect(missing_oauth_complete_input.exit_code != 0,
        "auth oauth-complete should reject missing callback_url and explicit empty token_endpoint");
    Expect(missing_oauth_complete_input.output.find("oauth_input_error command=oauth-complete provider=gemini") != std::string::npos,
        "auth oauth-complete should print a structured missing-input diagnostic");
    Expect(missing_oauth_complete_input.output.find("missing_fields=\"callback_url,token_endpoint\"") != std::string::npos,
        "auth oauth-complete should name missing callback_url and token_endpoint fields");
    Expect(missing_oauth_complete_input.output.find("endpoint_status=available") != std::string::npos,
        "auth oauth-complete missing-input diagnostics should include endpoint status");

#ifdef _WIN32
    const auto old_path = ReadEnvForTest("PATH").value_or("");
    const auto bin_dir = workspace / "bin";
    WriteCurlTokenFixture(bin_dir);
    SetEnvForTest("PATH", bin_dir.string() + PathListSeparatorForTest() + old_path);
    const auto oauth_complete = RunAgentos(workspace, {
        "auth", "oauth-complete", "gemini",
        "callback_url=http://127.0.0.1:48177/callback?code=auth-code&state=state-123",
        "state=state-123",
        "code_verifier=verifier",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "client_id=client-id",
        "profile=cli-oauth",
        "account_label=cli@example.test"});
    Expect(oauth_complete.exit_code == 0, "auth oauth-complete should exchange tokens and persist session on Windows");
    Expect(oauth_complete.output.find("gemini profile=cli-oauth") != std::string::npos,
        "auth oauth-complete should print persisted provider and profile");
    Expect(oauth_complete.output.find("profile=cli-oauth") != std::string::npos,
        "auth oauth-complete should print persisted profile");
    Expect(oauth_complete.output.find("source=agentos") != std::string::npos,
        "auth oauth-complete should persist an AgentOS-managed session");

    const auto oauth_login = RunAgentos(workspace, {
        "auth", "oauth-login", "gemini",
        "callback_url=http://127.0.0.1:48177/callback?code=login-code&state=fixed-login-state",
        "state=fixed-login-state",
        "code_verifier=fixed-login-verifier",
        "redirect_uri=http://127.0.0.1:48177/callback",
        "client_id=client-id",
        "profile=cli-oauth-login",
        "account_label=login@example.test"});
    SetEnvForTest("PATH", old_path);
    Expect(oauth_login.exit_code == 0, "auth oauth-login should complete a callback_url OAuth login on Windows");
    Expect(oauth_login.output.find("oauth_start provider=gemini profile=cli-oauth-login") != std::string::npos,
        "auth oauth-login should print the generated OAuth start details");
    Expect(oauth_login.output.find("oauth_callback success=true code=login-code") != std::string::npos,
        "auth oauth-login should validate and print the OAuth callback");
    Expect(oauth_login.output.find("gemini profile=cli-oauth-login") != std::string::npos,
        "auth oauth-login should print the persisted session");
    Expect(oauth_login.output.find("source=agentos") != std::string::npos,
        "auth oauth-login should persist an AgentOS-managed session");
#endif

    const auto old_qwen_key = ReadEnvForTest("QWEN_API_KEY").value_or("");
    SetEnvForTest("QWEN_API_KEY", "test-qwen-key");
    const auto qwen_login = RunAgentos(workspace, {
        "auth", "login", "qwen", "mode=api-key", "api_key_env=QWEN_API_KEY", "profile=team", "set_default=true"});
    SetEnvForTest("QWEN_API_KEY", old_qwen_key);
    Expect(qwen_login.exit_code == 0, "auth login should persist a Qwen API-key profile and allow setting it as default");

    const auto profiles_after_login = RunAgentos(workspace, {"auth", "profiles", "qwen"});
    Expect(profiles_after_login.exit_code == 0, "auth profiles should list persisted provider profiles after login");
    Expect(profiles_after_login.output.find("auth_profile provider=qwen profile=team default=true mode=api_key") != std::string::npos,
        "auth login set_default=true should mark the new profile as the provider default");

    const auto default_profile = RunAgentos(workspace, {"auth", "default-profile", "qwen", "profile=team"});
    Expect(default_profile.exit_code == 0, "auth default-profile should succeed");
    Expect(default_profile.output.find("qwen default_profile=team") != std::string::npos,
        "auth default-profile should confirm persisted profile");

    const auto profiles = RunAgentos(workspace, {"auth", "profiles", "qwen"});
    Expect(profiles.exit_code == 0, "auth profiles should list persisted provider profiles");
    Expect(profiles.output.find("auth_profile provider=qwen profile=team default=true mode=api_key") != std::string::npos,
        "auth profiles should include provider, profile, default marker, and mode");

    const auto status = RunAgentos(workspace, {"auth", "status", "qwen", "profile=team"});
    Expect(status.exit_code == 0, "auth status should succeed for a registered provider");
    Expect(status.output.find("qwen profile=team authenticated=false") != std::string::npos,
        "auth status should report the requested profile and unauthenticated state");

    const auto default_status = RunAgentos(workspace, {"auth", "status", "qwen"});
    Expect(default_status.exit_code == 0, "auth status should use the provider default profile when no profile is provided");
    Expect(default_status.output.find("qwen profile=team") != std::string::npos,
        "auth status should resolve the default profile set during login");

    // ---- login-interactive: stdin-driven wrapper around `auth login` ----
    const auto old_qwen_key2 = ReadEnvForTest("AGENTOS_QWEN_API_KEY").value_or("");
    SetEnvForTest("AGENTOS_QWEN_API_KEY", "test-interactive-key");
    // Stdin script: empty mode answer (default api-key), empty env answer
    // (default AGENTOS_QWEN_API_KEY), profile=interactive, set_default=Y.
    const std::string interactive_stdin = "\n\ninteractive\nY\n";
    const auto interactive = RunAgentosWithStdin(
        workspace,
        {"auth", "login-interactive", "provider=qwen"},
        interactive_stdin);
    SetEnvForTest("AGENTOS_QWEN_API_KEY", old_qwen_key2);
    Expect(interactive.exit_code == 0,
        "auth login-interactive should drive `auth login` to success when stdin defaults are accepted");
    Expect(
        interactive.output.find(
            "interactive_login provider=qwen mode=api_key profile=interactive set_default=true")
            != std::string::npos,
        "auth login-interactive should print a final summary line with provider/mode/profile/set_default");
    Expect(interactive.output.find("origin=stub") != std::string::npos,
        "auth login-interactive should surface OAuth defaults origin metadata for the chosen provider");

    const auto interactive_status = RunAgentos(workspace, {"auth", "status", "qwen", "profile=interactive"});
    Expect(interactive_status.exit_code == 0,
        "auth status should succeed for the profile created by login-interactive");
    Expect(interactive_status.output.find("qwen profile=interactive") != std::string::npos,
        "auth login-interactive should persist a real session via the existing AuthManager path");
}

// Verifies that `agentos run target=<provider> profile=<name>` and
// `agentos subagents run agents=<provider> profile=<name>` propagate the
// per-task auth profile override all the way to the provider adapter.
//
// Strategy: register a default Qwen API-key profile so QwenAgent::healthy()
// succeeds (a precondition for both the named-target router path and the
// SubagentManager::run_one health gate). Then dispatch with an *unconfigured*
// profile name. The adapter's session lookup will miss, producing an
// AuthUnavailable error whose message embeds the requested profile name —
// observable proof that profile= flowed through TaskRequest -> AgentTask /
// AgentInvocation -> adapter, rather than silently defaulting.
//
// curl must be on PATH for QwenAgent::healthy() to return true; CI runners
// (Windows/Ubuntu) and dev machines satisfy this. If a future environment
// strips curl this test will fail loudly at the healthy() gate, signaling the
// missing prerequisite rather than silently skipping the override check.
void TestRunAuthProfileOverride() {
    const auto workspace = FreshWorkspace("auth_profile_override");

    // Register a default Qwen profile so the adapter is healthy and selectable.
    // The env var is captured/restored around the login call so unrelated
    // tests don't see lingering state.
    const auto saved_qwen_key = ReadEnvForTest("QWEN_API_KEY").value_or("");
    SetEnvForTest("QWEN_API_KEY", "test-default-key");
    const auto login_result = RunAgentos(workspace, {
        "auth", "login", "qwen", "mode=api-key", "api_key_env=QWEN_API_KEY",
        "profile=default", "set_default=true"});
    SetEnvForTest("QWEN_API_KEY", saved_qwen_key);
    Expect(login_result.exit_code == 0,
        "auth login should register a default qwen profile so the adapter reports healthy");

    // `agentos run` honors profile= as the per-task override. Targeting an
    // unconfigured profile should reach the adapter and fail with the
    // requested profile name in the error message.
    const auto run_result = RunAgentos(workspace, {
        "run", "analysis", "target=qwen", "profile=ghost-profile",
        "objective=verify_profile_override"});
    Expect(run_result.output.find("error_code: AuthUnavailable") != std::string::npos,
        "agentos run target=qwen profile=ghost-profile should surface AuthUnavailable");
    Expect(run_result.output.find("ghost-profile") != std::string::npos,
        "agentos run profile= must reach the adapter (error_message should mention the requested profile)");

    // The auth_profile= alias must behave identically to profile=.
    const auto alias_result = RunAgentos(workspace, {
        "run", "analysis", "target=qwen", "auth_profile=ghost-alias",
        "objective=verify_alias"});
    Expect(alias_result.output.find("error_code: AuthUnavailable") != std::string::npos,
        "agentos run target=qwen auth_profile=ghost-alias should surface AuthUnavailable");
    Expect(alias_result.output.find("ghost-alias") != std::string::npos,
        "auth_profile= alias should propagate the same way profile= does");

    // `agentos subagents run` must propagate profile= through the
    // SubagentManager into the AgentInvocation. The orchestrator wraps the
    // per-agent failure inside its aggregate JSON output (top-level error is
    // SubagentFailure), so the AuthUnavailable signal appears inline as
    // `"error_code":"AuthUnavailable"` alongside the requested profile name.
    const auto subagents_result = RunAgentos(workspace, {
        "subagents", "run", "agents=qwen", "mode=sequential",
        "profile=ghost-subagent", "objective=verify_subagent_profile"});
    Expect(subagents_result.output.find("AuthUnavailable") != std::string::npos,
        "subagents run agents=qwen profile=ghost-subagent should surface AuthUnavailable in agent_outputs");
    Expect(subagents_result.output.find("ghost-subagent") != std::string::npos,
        "subagents run profile= must reach the adapter via SubagentManager");

    // Sanity check: omitting profile= falls back to the default profile and
    // therefore must NOT mention any of the override names — confirms the
    // override is targeted, not a global pollution.
    const auto default_run = RunAgentos(workspace, {
        "run", "analysis", "target=qwen", "objective=verify_default_profile"});
    Expect(default_run.output.find("ghost-profile") == std::string::npos,
        "agentos run without profile= must not leak prior override names");
    Expect(default_run.output.find("ghost-alias") == std::string::npos,
        "agentos run without profile= must not leak prior override names");
    Expect(default_run.output.find("ghost-subagent") == std::string::npos,
        "agentos run without profile= must not leak prior override names");
}

void TestInteractiveFreeFormDispatch() {
    const auto old_path = ReadEnvForTest("PATH").value_or("");

    {
        const auto workspace = FreshWorkspace("interactive_development_dispatch");
        const auto empty_bin = workspace / "empty-bin";
        std::filesystem::create_directories(empty_bin);
        SetEnvForTest("PATH", empty_bin.string());

        const auto result = RunAgentosWithStdin(
            workspace,
            {"interactive"},
            "please build a small command line tool\nexit\n");
        Expect(result.exit_code == 0, "interactive development-shaped input should exit cleanly");
        Expect(result.output.find("(route: chat_agent") != std::string::npos,
            "interactive development-shaped free-form text should route to main first");
        Expect(result.output.find("mode=sync") != std::string::npos,
            "main-first development-shaped route should declare sync mode");
        Expect(result.output.find("main-agent is not configured") != std::string::npos,
            "main-first development-shaped route should preserve the main-agent setup hint");
        const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
        Expect(audit.find("development_request") == std::string::npos,
            "interactive development-shaped text should not preemptively invoke development_request");
    }

    {
        const auto workspace = FreshWorkspace("interactive_research_dispatch");
        const auto empty_bin = workspace / "empty-bin";
        std::filesystem::create_directories(empty_bin);
        SetEnvForTest("PATH", empty_bin.string());

        const auto result = RunAgentosWithStdin(
            workspace,
            {"interactive"},
            "please research current provider integration details\nexit\n");
        Expect(result.exit_code == 0, "interactive research-shaped input should exit cleanly");
        Expect(result.output.find("(route: chat_agent") != std::string::npos,
            "interactive research-shaped free-form text should route to main first");
        Expect(result.output.find("mode=sync") != std::string::npos,
            "main-first research-shaped route should declare sync mode");
        Expect(result.output.find("main-agent is not configured") != std::string::npos,
            "main-first research-shaped route should preserve the main-agent setup hint");
        const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
        Expect(audit.find("research_request") == std::string::npos,
            "interactive research-shaped text should not preemptively invoke research_request");
    }

    {
        const auto workspace = FreshWorkspace("interactive_chat_dispatch");
        SetEnvForTest("PATH", old_path);

        const auto result = RunAgentosWithStdin(
            workspace,
            {"interactive"},
            "hello there\nexit\n");
        Expect(result.exit_code == 0, "interactive chat fallback should exit cleanly");
        Expect(result.output.find("(route: chat_agent") != std::string::npos,
            "interactive non-classified free-form text should still fall through to chat");
        Expect(result.output.find("mode=sync") != std::string::npos,
            "interactive chat route should declare sync execution mode");
        Expect(result.output.find("main-agent is not configured") != std::string::npos,
            "interactive chat fallback should preserve the existing main-agent setup hint");
    }

#ifndef _WIN32
    {
        const auto workspace = FreshWorkspace("interactive_utf8_pty_dispatch");
        SetEnvForTest("PATH", old_path);

        const auto result = RunAgentosInPtyWithInput(
            workspace,
            {"interactive"},
            "你好\nexit\n");
        Expect(result.exit_code == 0, "interactive UTF-8 pty dispatch should exit cleanly");
        Expect(result.output.find("你好") != std::string::npos,
            "interactive raw terminal input should preserve UTF-8 text");
        Expect(result.output.find("(route: chat_agent") != std::string::npos,
            "interactive UTF-8 free-form text should be routed instead of dropped as an empty line");
    }
#endif

    SetEnvForTest("PATH", old_path);
}

void TestInteractiveMainRouteActionLoop() {
    const auto old_path = ReadEnvForTest("PATH").value_or("");
    const auto old_api_key = ReadEnvForTest("AGENTOS_TEST_MAIN_KEY").value_or("");
    const auto workspace = FreshWorkspace("interactive_main_route_action_loop");
    const auto bin_dir = workspace / "bin";
    const auto counter_path = workspace / "main_route_counter.txt";
    WriteMainRouteActionCurlFixture(bin_dir, counter_path);
    SetEnvForTest("PATH", bin_dir.string() + PathListSeparatorForTest() + old_path);
    SetEnvForTest("AGENTOS_TEST_MAIN_KEY", "fixture-key");

    const auto set_main = RunAgentos(workspace, {
        "main-agent", "set",
        "provider=openai-chat",
        "base_url=https://main.fixture.test/v1",
        "model=fixture-main",
        "api_key_env=AGENTOS_TEST_MAIN_KEY"});
    Expect(set_main.exit_code == 0, "main-agent fixture config should save");

    const auto result = RunAgentosWithStdin(
        workspace,
        {"interactive"},
        "inspect host through main\nexit\n");
    Expect(result.exit_code == 0, "interactive main route action loop should exit cleanly");
    Expect(result.output.find("(main requested call_capability target=skill:host_info)") != std::string::npos,
        "REPL should detect and announce main route action");
    Expect(result.output.find("route action synthesis complete") != std::string::npos,
        "REPL should feed route result back to main for synthesis");
    Expect(ReadTextFile(counter_path).find("2") != std::string::npos,
        "main curl fixture should be called twice: action then synthesis");

    const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
    Expect(audit.find("host_info") != std::string::npos,
        "route action execution should invoke host_info through normal task audit");

    SetEnvForTest("PATH", old_path);
    SetEnvForTest("AGENTOS_TEST_MAIN_KEY", old_api_key);
}

void TestInteractiveMainRouteActionValidationLoop() {
    const auto old_path = ReadEnvForTest("PATH").value_or("");
    const auto old_api_key = ReadEnvForTest("AGENTOS_TEST_MAIN_KEY").value_or("");
    const auto workspace = FreshWorkspace("interactive_main_route_action_validation_loop");
    const auto bin_dir = workspace / "bin";
    const auto counter_path = workspace / "main_route_validation_counter.txt";
    WriteMainRouteActionMissingInputCurlFixture(bin_dir, counter_path);
    SetEnvForTest("PATH", bin_dir.string() + PathListSeparatorForTest() + old_path);
    SetEnvForTest("AGENTOS_TEST_MAIN_KEY", "fixture-key");

    const auto set_main = RunAgentos(workspace, {
        "main-agent", "set",
        "provider=openai-chat",
        "base_url=https://main.fixture.test/v1",
        "model=fixture-main",
        "api_key_env=AGENTOS_TEST_MAIN_KEY"});
    Expect(set_main.exit_code == 0, "main-agent validation fixture config should save");

    const auto result = RunAgentosWithStdin(
        workspace,
        {"interactive"},
        "search news through main\nexit\n");
    Expect(result.exit_code == 0, "interactive main route action validation loop should exit cleanly");
    Expect(result.output.find("(main requested call_capability target=skill:news_search)") != std::string::npos,
        "REPL should announce the invalid route action target");
    Expect(result.output.find("route action validation synthesis complete") != std::string::npos,
        "REPL should feed validation failure back to main for synthesis");
    Expect(result.output.find("success: false") == std::string::npos,
        "validation failure should not be dumped as raw task output before synthesis");
    Expect(ReadTextFile(counter_path).find("2") != std::string::npos,
        "main curl fixture should be called twice: invalid action then synthesis");

    SetEnvForTest("PATH", old_path);
    SetEnvForTest("AGENTOS_TEST_MAIN_KEY", old_api_key);
}

void TestInteractiveMainRouteActionContextAfterClarification() {
    const auto old_path = ReadEnvForTest("PATH").value_or("");
    const auto old_api_key = ReadEnvForTest("AGENTOS_TEST_MAIN_KEY").value_or("");
    const auto workspace = FreshWorkspace("interactive_main_route_action_context_loop");
    const auto bin_dir = workspace / "bin";
    const auto counter_path = workspace / "main_route_context_counter.txt";
    WriteMainRouteActionContextCurlFixture(bin_dir, counter_path);
    SetEnvForTest("PATH", bin_dir.string() + PathListSeparatorForTest() + old_path);
    SetEnvForTest("AGENTOS_TEST_MAIN_KEY", "fixture-key");

    const auto set_main = RunAgentos(workspace, {
        "main-agent", "set",
        "provider=openai-chat",
        "base_url=https://main.fixture.test/v1",
        "model=fixture-main",
        "api_key_env=AGENTOS_TEST_MAIN_KEY"});
    Expect(set_main.exit_code == 0, "main-agent context fixture config should save");

    const auto result = RunAgentosWithStdin(
        workspace,
        {"interactive"},
        "search news through main\nAI browser\nthanks\nexit\n");
    Expect(result.exit_code == 0, "interactive main route action context loop should exit cleanly");
    Expect(result.output.find("please provide query") != std::string::npos,
        "first route validation synthesis should ask for the missing query");
    Expect(result.output.find("context ok") != std::string::npos,
        "third turn should receive clean context after successful retry");
    Expect(result.output.find("context leaked internal route result") == std::string::npos,
        "recent chat context should not contain the internal route result prompt");
    Expect(result.output.find("context missing pending route action") == std::string::npos,
        "second turn should include pending route action context");
    Expect(result.output.find("pending leaked after success") == std::string::npos,
        "pending route action should clear after successful retry");
    Expect(ReadTextFile(counter_path).find("4") != std::string::npos,
        "main curl fixture should be called for invalid action, clarification, retry, and final synthesis");

    const auto audit = ReadTextFile(workspace / "runtime" / "audit.log");
    Expect(audit.find("\"query\":\"AI browser\"") != std::string::npos ||
               audit.find("\"query\": \"AI browser\"") != std::string::npos ||
               audit.find("AI browser") != std::string::npos,
        "successful retry should invoke news_search with the supplied query");

    SetEnvForTest("PATH", old_path);
    SetEnvForTest("AGENTOS_TEST_MAIN_KEY", old_api_key);
}

void TestAutoDevCommands() {
    const auto workspace = FreshWorkspace("autodev_cli");
    const auto target = workspace / "target_app";
    InitGitRepoForCliTest(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    const auto submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Fix login 500",
        "skill_pack_path=" + skill_pack.string()});
    Expect(submit.exit_code == 0, "autodev submit should succeed");
    Expect(submit.output.find("AutoDev job submitted") != std::string::npos,
        "autodev submit should print success heading");
    Expect(submit.output.find("isolation_status:   pending") != std::string::npos,
        "autodev submit should report pending isolation");
    Expect(submit.output.find("next_action:        prepare_workspace") != std::string::npos,
        "autodev submit should report prepare_workspace next action");
    Expect(submit.output.find("Workspace is not ready yet") != std::string::npos,
        "autodev submit should make workspace readiness explicit");

    const auto job_id = ExtractLineValue(submit.output, "job_id:");
    Expect(job_id.rfind("autodev-", 0) == 0, "autodev submit should print generated job id");
    const auto job_dir = workspace / "runtime" / "autodev" / "jobs" / job_id;
    Expect(std::filesystem::exists(job_dir / "job.json"),
        "autodev submit should create job.json in AgentOS runtime");
    Expect(std::filesystem::exists(job_dir / "events.ndjson"),
        "autodev submit should create events.ndjson in AgentOS runtime");

    const auto job_json = ReadTextFile(job_dir / "job.json");
    Expect(job_json.find("\"status\": \"submitted\"") != std::string::npos,
        "job.json should record submitted status");
    Expect(job_json.find("\"phase\": \"workspace_preparing\"") != std::string::npos,
        "job.json should record workspace_preparing phase");
    Expect(job_json.find("\"isolation_status\": \"pending\"") != std::string::npos,
        "job.json should record pending isolation");
    Expect(job_json.find("\"next_action\": \"prepare_workspace\"") != std::string::npos,
        "job.json should record prepare_workspace next action");
    Expect(job_json.find("\"status\": \"declared\"") != std::string::npos,
        "job.json should record declared skill pack when skill_pack_path is provided");
    Expect(job_json.find(skill_pack.string()) != std::string::npos,
        "job.json should record skill_pack_path");

    const auto events = ReadTextFile(job_dir / "events.ndjson");
    Expect(events.find("\"type\":\"autodev.job.submitted\"") != std::string::npos,
        "events.ndjson should record submit event");
    Expect(events.find("\"planned_worktree_path\"") != std::string::npos,
        "events.ndjson should record planned worktree path");

    const auto planned_worktree = ExtractLineValue(submit.output, "job_worktree_path:");
    const auto planned_path = planned_worktree.substr(0, planned_worktree.find(" "));
    Expect(!planned_path.empty(), "autodev submit should print planned worktree path");
    Expect(!std::filesystem::exists(planned_path),
        "autodev submit should not create planned worktree path");
    Expect(!std::filesystem::exists(target / "runtime" / "autodev"),
        "autodev submit should not write runtime facts into target repo");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "autodev submit should not write docs/goal into target repo");

    const auto status = RunAgentos(workspace, {"autodev", "status", "job_id=" + job_id});
    Expect(status.exit_code == 0, "autodev status should read submitted job");
    Expect(status.output.find("Job: " + job_id) != std::string::npos,
        "autodev status should print job id");
    Expect(status.output.find("Status: submitted") != std::string::npos,
        "autodev status should print current status");
    Expect(status.output.find("status:            pending") != std::string::npos,
        "autodev status should print pending isolation");
    Expect(status.output.find("agentos autodev prepare_workspace job_id=" + job_id) != std::string::npos ||
               status.output.find("agentos autodev prepare-workspace job_id=" + job_id) != std::string::npos,
        "autodev status should print prepare workspace next action");

    const auto prepare = RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + job_id});
    Expect(prepare.exit_code == 0, "autodev prepare-workspace should succeed for clean git repo");
    Expect(prepare.output.find("AutoDev workspace prepared") != std::string::npos,
        "autodev prepare-workspace should print success heading");
    Expect(prepare.output.find("isolation_status:      ready") != std::string::npos,
        "autodev prepare-workspace should report ready isolation");
    Expect(std::filesystem::exists(std::filesystem::path(planned_path) / ".git"),
        "autodev prepare-workspace should create planned git worktree");

    const auto load_skill_pack = RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + job_id});
    Expect(load_skill_pack.exit_code == 0, "autodev load-skill-pack should succeed for complete fixture");
    Expect(load_skill_pack.output.find("AutoDev skill pack loaded") != std::string::npos,
        "autodev load-skill-pack should print success heading");
    Expect(load_skill_pack.output.find("skill_pack_status:  loaded") != std::string::npos,
        "autodev load-skill-pack should report loaded status");
    Expect(std::filesystem::exists(job_dir / "artifacts" / "skill_pack.snapshot.json"),
        "autodev load-skill-pack should write runtime skill pack snapshot");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "autodev load-skill-pack should not generate docs/goal in target repo");

    const auto loaded_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + job_id});
    Expect(loaded_status.exit_code == 0, "autodev status should read loaded skill pack job");
    Expect(loaded_status.output.find("status: loaded") != std::string::npos,
        "autodev status should show loaded skill pack");
    Expect(loaded_status.output.find("hash:") != std::string::npos,
        "autodev status should show skill pack manifest hash");

    const auto generate_docs = RunAgentos(workspace, {"autodev", "generate-goal-docs", "job_id=" + job_id});
    Expect(generate_docs.exit_code == 0, "autodev generate-goal-docs should succeed after workspace and skill pack are ready");
    Expect(generate_docs.output.find("AutoDev goal docs generated") != std::string::npos,
        "autodev generate-goal-docs should print success heading");
    Expect(generate_docs.output.find("files_written: 16") != std::string::npos,
        "autodev generate-goal-docs should report skeleton file count");
    Expect(std::filesystem::exists(std::filesystem::path(planned_path) / "docs" / "goal" / "GOAL.md"),
        "autodev generate-goal-docs should write GOAL.md under job worktree");
    Expect(std::filesystem::exists(std::filesystem::path(planned_path) / "docs" / "goal" / "AUTODEV_SPEC.json"),
        "autodev generate-goal-docs should write AUTODEV_SPEC.json under job worktree");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "autodev generate-goal-docs should not write docs/goal into target repo");

    const auto generated_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + job_id});
    Expect(generated_status.exit_code == 0, "autodev status should read generated goal docs job");
    Expect(generated_status.output.find("Phase: requirements_grilling") != std::string::npos,
        "autodev status should show requirements_grilling after goal docs generation");
    Expect(generated_status.output.find("agentos autodev validate-spec job_id=" + job_id) != std::string::npos,
        "autodev status should show validate_spec next action");

    const auto validate_spec = RunAgentos(workspace, {"autodev", "validate-spec", "job_id=" + job_id});
    Expect(validate_spec.exit_code == 0, "autodev validate-spec should succeed for generated candidate spec");
    Expect(validate_spec.output.find("AutoDev spec validated") != std::string::npos,
        "autodev validate-spec should print success heading");
    Expect(validate_spec.output.find("status:        awaiting_approval") != std::string::npos,
        "autodev validate-spec should stop at awaiting approval");
    Expect(validate_spec.output.find("approval_gate: before_code_execution") != std::string::npos,
        "autodev validate-spec should report before_code_execution gate");
    Expect(validate_spec.output.find("spec_revision: rev-001") != std::string::npos,
        "autodev validate-spec should report first spec revision");
    const auto spec_hash = ExtractLineValue(validate_spec.output, "spec_hash:");
    Expect(spec_hash.size() == 64, "autodev validate-spec should print sha256 spec hash");
    Expect(std::filesystem::exists(job_dir / "spec_revisions" / "rev-001.normalized.json"),
        "autodev validate-spec should write normalized spec snapshot under runtime store");
    Expect(std::filesystem::exists(job_dir / "spec_revisions" / "rev-001.sha256"),
        "autodev validate-spec should write spec hash under runtime store");
    Expect(std::filesystem::exists(job_dir / "spec_revisions" / "rev-001.status.json"),
        "autodev validate-spec should write spec revision status under runtime store");

    const auto validated_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + job_id});
    Expect(validated_status.exit_code == 0, "autodev status should read validated spec job");
    Expect(validated_status.output.find("Status: awaiting_approval") != std::string::npos,
        "autodev status should show awaiting approval after spec validation");
    Expect(validated_status.output.find("Approval gate: before_code_execution") != std::string::npos,
        "autodev status should show before_code_execution after spec validation");
    Expect(validated_status.output.find("revision:       rev-001") != std::string::npos,
        "autodev status should show spec revision");
    Expect(validated_status.output.find("agentos autodev approve-spec job_id=" + job_id + " spec_hash=" + spec_hash) != std::string::npos,
        "autodev status should show hash-bound approve_spec next action");

    const auto approve_empty = RunAgentos(workspace, {"autodev", "approve-spec", "job_id=" + job_id, "spec_hash=" + spec_hash});
    Expect(approve_empty.exit_code != 0, "autodev approve-spec should block empty generated task skeleton");
    Expect(approve_empty.output.find("tasks must not be empty") != std::string::npos,
        "autodev approve-spec should explain empty tasks blocker");
    const auto blocked_spec_summary = RunAgentos(workspace, {"autodev", "summary", "job_id=" + job_id});
    Expect(blocked_spec_summary.exit_code == 0,
        "autodev summary should work for blocked jobs before tasks.json exists");
    Expect(blocked_spec_summary.output.find("recovery:      Fix docs/goal/AUTODEV_SPEC.json") != std::string::npos,
        "autodev summary should show spec recovery guidance for approval-blocked jobs");
    {
        std::ofstream spec(std::filesystem::path(planned_path) / "docs" / "goal" / "AUTODEV_SPEC.json",
            std::ios::binary | std::ios::trunc);
        spec
            << "{\n"
            << "  \"schema_version\": \"1.0.0\",\n"
            << "  \"generated_by\": \"agentos-cli-test\",\n"
            << "  \"generated_by_skill_pack\": \"maxenergy/skills\",\n"
            << "  \"agentos_min_version\": \"0.1.0\",\n"
            << "  \"created_at\": \"2026-05-06T00:00:00Z\",\n"
            << "  \"objective\": \"Fix login 500\",\n"
            << "  \"mode\": \"feature\",\n"
            << "  \"source_of_truth\": [\"docs/goal/REQUIREMENTS.md\"],\n"
            << "  \"tasks\": [\n"
            << "    {\n"
            << "      \"task_id\": \"task-001\",\n"
            << "      \"title\": \"Document login fix\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"true\",\n"
            << "      \"acceptance\": [\"README.md remains present\"]\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
    }
    const auto recover_spec = RunAgentos(workspace, {"autodev", "recover-blocked", "job_id=" + job_id});
    Expect(recover_spec.exit_code == 0,
        "autodev recover-blocked should rerun spec validation after AUTODEV_SPEC is fixed");
    Expect(recover_spec.output.find("attempted_action: validate-spec") != std::string::npos,
        "autodev recover-blocked should report spec validation recovery action");
    Expect(recover_spec.output.find("status:      awaiting_approval") != std::string::npos,
        "autodev recover-blocked should move fixed spec back to awaiting approval");
    const auto recover_awaiting_approval = RunAgentos(workspace, {"autodev", "recover-blocked", "job_id=" + job_id});
    Expect(recover_awaiting_approval.exit_code != 0,
        "autodev recover-blocked should not approve specs implicitly");
    Expect(recover_awaiting_approval.output.find("spec approval requires explicit approve-spec") != std::string::npos,
        "autodev recover-blocked should explain explicit approval requirement");
    const auto tasks_before_approval = RunAgentos(workspace, {"autodev", "tasks", "job_id=" + job_id});
    Expect(tasks_before_approval.exit_code != 0,
        "autodev tasks should fail before runtime tasks are materialized");
    Expect(tasks_before_approval.output.find("AutoDev tasks not found") != std::string::npos,
        "autodev tasks should explain missing tasks.json before approval");
    const auto events_result = RunAgentos(workspace, {"autodev", "events", "job_id=" + job_id});
    Expect(events_result.exit_code == 0, "autodev events should read append-only job history");
    Expect(events_result.output.find("AutoDev events") != std::string::npos,
        "autodev events should print heading");
    Expect(events_result.output.find("autodev.job.submitted") != std::string::npos,
        "autodev events should include submit event");
    Expect(events_result.output.find("autodev.spec.approval_blocked") != std::string::npos,
        "autodev events should include approval blocked event");

    const auto dirty_target = workspace / "dirty_target";
    InitGitRepoForCliTest(dirty_target);
    {
        std::ofstream dirty_file(dirty_target / "README.md", std::ios::binary | std::ios::app);
        dirty_file << "dirty\n";
    }
    const auto dirty_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + dirty_target.string(),
        "objective=Recover dirty workspace",
        "skill_pack_path=" + skill_pack.string()});
    Expect(dirty_submit.exit_code == 0,
        "autodev submit should allow dirty target recovery fixture setup");
    const auto dirty_job_id = ExtractLineValue(dirty_submit.output, "job_id:");
    const auto dirty_prepare = RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + dirty_job_id});
    Expect(dirty_prepare.exit_code != 0,
        "autodev prepare-workspace should block dirty target recovery fixture");
    const auto dirty_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + dirty_job_id});
    Expect(dirty_status.output.find("Recovery:") != std::string::npos &&
               dirty_status.output.find("recover-blocked job_id=" + dirty_job_id) != std::string::npos,
        "autodev status should show workspace recovery guidance");
    (void)std::system((std::string("git -C ") + QuoteShellArg(dirty_target.string()) + " add README.md").c_str());
    (void)std::system((std::string("git -C ") + QuoteShellArg(dirty_target.string()) +
                       " commit -m recover-dirty >/dev/null 2>&1").c_str());
    const auto recover_dirty = RunAgentos(workspace, {"autodev", "recover-blocked", "job_id=" + dirty_job_id});
    Expect(recover_dirty.exit_code == 0,
        "autodev recover-blocked should rerun prepare-workspace after dirty target is fixed");
    Expect(recover_dirty.output.find("attempted_action: prepare-workspace") != std::string::npos,
        "autodev recover-blocked should report workspace recovery action");
    Expect(recover_dirty.output.find("status:      running") != std::string::npos,
        "autodev recover-blocked should unblock clean workspace jobs");

    const auto missing_skill_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Recover missing skill pack"});
    Expect(missing_skill_submit.exit_code == 0,
        "autodev submit should allow missing skill pack recovery fixture setup");
    const auto missing_skill_job_id = ExtractLineValue(missing_skill_submit.output, "job_id:");
    Expect(RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + missing_skill_job_id}).exit_code == 0,
        "autodev prepare-workspace should prepare missing skill recovery fixture");
    const auto missing_skill_load = RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + missing_skill_job_id});
    Expect(missing_skill_load.exit_code != 0,
        "autodev load-skill-pack should block when skill_pack_path is missing");
    const auto missing_skill_summary = RunAgentos(workspace, {"autodev", "summary", "job_id=" + missing_skill_job_id});
    Expect(missing_skill_summary.exit_code == 0,
        "autodev summary should work for missing skill pack blocked jobs");
    Expect(missing_skill_summary.output.find("skill_pack_path=<path>") != std::string::npos,
        "autodev summary should show missing skill pack recovery guidance");
    const auto recover_skill = RunAgentos(workspace, {
        "autodev",
        "recover-blocked",
        "job_id=" + missing_skill_job_id,
        "skill_pack_path=" + skill_pack.string()});
    Expect(recover_skill.exit_code == 0,
        "autodev recover-blocked should load provided skill pack path");
    Expect(recover_skill.output.find("attempted_action: load-skill-pack") != std::string::npos,
        "autodev recover-blocked should report skill pack recovery action");
    Expect(recover_skill.output.find("status:      running") != std::string::npos,
        "autodev recover-blocked should unblock skill pack jobs");

    const auto executable_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Execute approved task preflight",
        "skill_pack_path=" + skill_pack.string()});
    Expect(executable_submit.exit_code == 0, "autodev submit should support a second executable fixture job");
    const auto executable_job_id = ExtractLineValue(executable_submit.output, "job_id:");
    const auto executable_planned_worktree = ExtractLineValue(executable_submit.output, "job_worktree_path:");
    const auto executable_planned_path = executable_planned_worktree.substr(0, executable_planned_worktree.find(" "));
    const auto executable_job_dir = workspace / "runtime" / "autodev" / "jobs" / executable_job_id;

    Expect(RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + executable_job_id}).exit_code == 0,
        "autodev prepare-workspace should prepare executable fixture job");
    Expect(RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + executable_job_id}).exit_code == 0,
        "autodev load-skill-pack should load executable fixture job");
    Expect(RunAgentos(workspace, {"autodev", "generate-goal-docs", "job_id=" + executable_job_id}).exit_code == 0,
        "autodev generate-goal-docs should generate executable fixture docs");
    {
        std::ofstream spec(std::filesystem::path(executable_planned_path) / "docs" / "goal" / "AUTODEV_SPEC.json",
            std::ios::binary | std::ios::trunc);
        spec
            << "{\n"
            << "  \"schema_version\": \"1.0.0\",\n"
            << "  \"generated_by\": \"agentos-cli-test\",\n"
            << "  \"generated_by_skill_pack\": \"maxenergy/skills\",\n"
            << "  \"agentos_min_version\": \"0.1.0\",\n"
            << "  \"created_at\": \"2026-05-06T00:00:00Z\",\n"
            << "  \"objective\": \"Execute approved task preflight\",\n"
            << "  \"mode\": \"feature\",\n"
            << "  \"source_of_truth\": [\"docs/goal/REQUIREMENTS.md\"],\n"
            << "  \"tasks\": [\n"
            << "    {\n"
            << "      \"task_id\": \"task-001\",\n"
            << "      \"title\": \"Update README through execution adapter\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"true\",\n"
            << "      \"acceptance\": [\"README.md remains present\"]\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
    }
    const auto executable_validate = RunAgentos(workspace, {"autodev", "validate-spec", "job_id=" + executable_job_id});
    Expect(executable_validate.exit_code == 0,
        "autodev validate-spec should validate executable fixture spec");
    const auto executable_spec_hash = ExtractLineValue(executable_validate.output, "spec_hash:");
    const auto executable_approve = RunAgentos(workspace, {
        "autodev",
        "approve-spec",
        "job_id=" + executable_job_id,
        "spec_hash=" + executable_spec_hash});
    Expect(executable_approve.exit_code == 0,
        "autodev approve-spec should approve executable fixture spec");
    const auto executable_tasks_result = RunAgentos(workspace, {"autodev", "tasks", "job_id=" + executable_job_id});
    Expect(executable_tasks_result.exit_code == 0,
        "autodev tasks should list materialized executable task");
    Expect(executable_tasks_result.output.find("retry:           0/3") != std::string::npos,
        "autodev tasks should show default retry counters");
    {
        std::ofstream lock(executable_job_dir / "job.lock", std::ios::binary | std::ios::trunc);
        lock << "held by test\n";
    }
    const auto locked_snapshot = RunAgentos(workspace, {
        "autodev",
        "snapshot-task",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(locked_snapshot.exit_code != 0,
        "mutating AutoDev job commands should fail while the job runtime lock is held");
    Expect(locked_snapshot.output.find("AutoDev job runtime lock") != std::string::npos,
        "runtime lock failures should explain the locked job");
    std::filesystem::remove(executable_job_dir / "job.lock");
    const auto codex_fixture = WriteAutoDevCodexCliFixture(workspace / "bin");
    const auto execute_next = RunAgentos(workspace, {
        "autodev",
        "execute-next-task",
        "job_id=" + executable_job_id,
        "codex_cli_command=" + codex_fixture.string()});
    Expect(execute_next.exit_code == 0,
        "autodev execute-next-task should run the configured Codex CLI command");
    Expect(execute_next.output.find("AutoDev execution completed") != std::string::npos,
        "autodev execute-next-task should print execution completion details");
    Expect(execute_next.output.find("task_id:            task-001") != std::string::npos,
        "autodev execute-next-task should select the first pending runtime task");
    Expect(execute_next.output.find("turn_id:            turn-001") != std::string::npos,
        "autodev execute-next-task should record the first execution turn");
    Expect(execute_next.output.find("turn_status:        completed") != std::string::npos,
        "autodev execute-next-task should record completed turn status");
    Expect(execute_next.output.find("exit_code:          0") != std::string::npos,
        "autodev execute-next-task should record the Codex CLI exit code");
    Expect(execute_next.output.find("snapshot_id:        snapshot-001") != std::string::npos,
        "autodev execute-next-task should print the snapshot id");
    Expect(execute_next.output.find("adapter_kind:       codex_cli") != std::string::npos,
        "autodev execute-next-task should expose the Codex CLI adapter kind");
    const auto executable_tasks = ReadTextFile(executable_job_dir / "tasks.json");
    Expect(executable_tasks.find("\"status\": \"pending\"") != std::string::npos,
        "execute-next-task should leave runtime task status pending until acceptance gate runs");
    const auto turns_result = RunAgentos(workspace, {"autodev", "turns", "job_id=" + executable_job_id});
    Expect(turns_result.exit_code == 0,
        "autodev turns should list execution turn records");
    Expect(turns_result.output.find("AutoDev turns") != std::string::npos,
        "autodev turns should print heading");
    Expect(turns_result.output.find("turn_id:           turn-001") != std::string::npos,
        "autodev turns should list the first synthetic turn id");
    Expect(turns_result.output.find("status:            completed") != std::string::npos,
        "autodev turns should show completed execution turn status");
    Expect(turns_result.output.find("adapter_kind:      codex_cli") != std::string::npos,
        "autodev turns should show adapter kind");
    Expect(turns_result.output.find("changed_files:     README.md") != std::string::npos,
        "autodev turns should show files changed by the Codex CLI fixture");
    Expect(turns_result.output.find("prompt_artifact:") != std::string::npos,
        "autodev turns should show prompt artifact path");
    Expect(turns_result.output.find("response_artifact:") != std::string::npos,
        "autodev turns should show response artifact path");
    Expect(std::filesystem::exists(executable_job_dir / "prompts" / "turn-001.md"),
        "execute-next-task should write prompt artifact under AgentOS runtime store");
    Expect(std::filesystem::exists(executable_job_dir / "responses" / "turn-001.md"),
        "execute-next-task should write response artifact under AgentOS runtime store");
    Expect(std::filesystem::exists(executable_job_dir / "snapshots.json"),
        "execute-next-task should write snapshots.json under AgentOS runtime store");
    Expect(std::filesystem::exists(executable_job_dir / "snapshots" / "snapshot-001.json"),
        "execute-next-task should write a per-snapshot artifact under AgentOS runtime store");
    const auto snapshots_result = RunAgentos(workspace, {"autodev", "snapshots", "job_id=" + executable_job_id});
    Expect(snapshots_result.exit_code == 0,
        "autodev snapshots should list recorded snapshot facts");
    Expect(snapshots_result.output.find("AutoDev snapshots") != std::string::npos,
        "autodev snapshots should print heading");
    Expect(snapshots_result.output.find("snapshot_id: snapshot-001") != std::string::npos,
        "autodev snapshots should include snapshot-001");
    const auto rollbacks_result = RunAgentos(workspace, {"autodev", "rollbacks", "job_id=" + executable_job_id});
    Expect(rollbacks_result.exit_code == 0,
        "autodev rollbacks should list rollback facts even before any rollback exists");
    Expect(rollbacks_result.output.find("AutoDev rollbacks") != std::string::npos,
        "autodev rollbacks should print heading");
    Expect(rollbacks_result.output.find("total:  0") != std::string::npos,
        "autodev rollbacks should show zero records before rollback commands run");
    Expect(rollbacks_result.output.find("does not modify the worktree") != std::string::npos,
        "autodev rollbacks should state that query is non-destructive");
    const auto verify_task = RunAgentos(workspace, {
        "autodev",
        "verify-task",
        "job_id=" + executable_job_id,
        "task_id=task-001",
        "related_turn_id=turn-001"});
    Expect(verify_task.exit_code == 0,
        "autodev verify-task should run task verify_command in job worktree");
    Expect(verify_task.output.find("AutoDev task verified") != std::string::npos,
        "autodev verify-task should print success heading");
    Expect(verify_task.output.find("verification_id: verify-001") != std::string::npos,
        "autodev verify-task should print verification id");
    Expect(verify_task.output.find("passed:          true") != std::string::npos,
        "autodev verify-task should report passed=true for true command");
    Expect(verify_task.output.find("AcceptanceGate was not run") != std::string::npos,
        "autodev verify-task should state that AcceptanceGate was not run");
    Expect(verify_task.output.find("verify_report:") != std::string::npos,
        "autodev verify-task should print VERIFY.md path");
    Expect(std::filesystem::exists(executable_job_dir / "verification.json"),
        "autodev verify-task should write verification.json under runtime store");
    Expect(std::filesystem::exists(executable_job_dir / "logs" / "verify-001.output.txt"),
        "autodev verify-task should write command output log under runtime store");
    const auto verify_report_path = std::filesystem::path(executable_planned_path) / "docs" / "goal" / "VERIFY.md";
    Expect(std::filesystem::exists(verify_report_path),
        "autodev verify-task should write VERIFY.md summary under job worktree");
    const auto verify_report = ReadTextFile(verify_report_path);
    Expect(verify_report.find("It is NOT the source of truth for task completion") != std::string::npos,
        "VERIFY.md should state summary-only authority boundary");
    Expect(verify_report.find("verify-001") != std::string::npos,
        "VERIFY.md should include verification id");
    const auto verifications_result = RunAgentos(workspace, {"autodev", "verifications", "job_id=" + executable_job_id});
    Expect(verifications_result.exit_code == 0,
        "autodev verifications should list recorded verification facts");
    Expect(verifications_result.output.find("AutoDev verifications") != std::string::npos,
        "autodev verifications should print heading");
    Expect(verifications_result.output.find("verification_id: verify-001") != std::string::npos,
        "autodev verifications should include verify-001");
    Expect(verifications_result.output.find("related_turn_id: turn-001") != std::string::npos,
        "autodev verifications should include related turn id");
    {
        std::ofstream readme(std::filesystem::path(executable_planned_path) / "README.md",
            std::ios::binary | std::ios::app);
        readme << "allowed cli change\n";
    }
    const auto diff_guard_pass = RunAgentos(workspace, {
        "autodev",
        "diff-guard",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(diff_guard_pass.exit_code == 0,
        "autodev diff-guard should pass for allowed file changes");
    Expect(diff_guard_pass.output.find("AutoDev diff guard checked") != std::string::npos,
        "autodev diff-guard should print heading");
    Expect(diff_guard_pass.output.find("diff_id: diff-001") != std::string::npos,
        "autodev diff-guard should print diff id");
    Expect(diff_guard_pass.output.find("passed:  true") != std::string::npos,
        "autodev diff-guard should report passed=true for allowed file");
    Expect(std::filesystem::exists(executable_job_dir / "diffs.json"),
        "autodev diff-guard should write diffs.json under runtime store");
    const auto acceptance_gate = RunAgentos(workspace, {
        "autodev",
        "acceptance-gate",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(acceptance_gate.exit_code == 0,
        "autodev acceptance-gate should pass when latest verification and diff guard passed");
    Expect(acceptance_gate.output.find("AutoDev acceptance gate checked") != std::string::npos,
        "autodev acceptance-gate should print heading");
    Expect(acceptance_gate.output.find("acceptance_id: acceptance-001") != std::string::npos,
        "autodev acceptance-gate should print acceptance id");
    Expect(acceptance_gate.output.find("passed:        true") != std::string::npos,
        "autodev acceptance-gate should report passed=true");
    Expect(acceptance_gate.output.find("task_status:   passed") != std::string::npos,
        "autodev acceptance-gate should mark the task passed");
    Expect(std::filesystem::exists(executable_job_dir / "acceptance.json"),
        "autodev acceptance-gate should write acceptance.json under runtime store");
    const auto acceptances = RunAgentos(workspace, {"autodev", "acceptances", "job_id=" + executable_job_id});
    Expect(acceptances.exit_code == 0,
        "autodev acceptances should list acceptance gate facts");
    Expect(acceptances.output.find("AutoDev acceptances") != std::string::npos,
        "autodev acceptances should print heading");
    Expect(acceptances.output.find("acceptance_id: acceptance-001") != std::string::npos,
        "autodev acceptances should include acceptance-001");
    Expect(acceptances.output.find("verification:  verify-001") != std::string::npos,
        "autodev acceptances should include linked verification id");
    const auto accepted_tasks = ReadTextFile(executable_job_dir / "tasks.json");
    Expect(accepted_tasks.find("\"status\": \"passed\"") != std::string::npos,
        "autodev acceptance-gate should persist passed task status");
    const auto accepted_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + executable_job_id});
    Expect(accepted_status.exit_code == 0,
        "autodev status should read job after acceptance gate");
    Expect(accepted_status.output.find("Status: running") != std::string::npos,
        "autodev acceptance-gate should not mark the job done");
    Expect(accepted_status.output.find("Phase: final_review") != std::string::npos,
        "autodev acceptance-gate should advance all-passed jobs to final_review");
    Expect(accepted_status.output.find("agentos autodev final-review job_id=" + executable_job_id) != std::string::npos,
        "autodev status should show final_review as the next action");
    Expect(accepted_status.output.find("passed: 1") != std::string::npos,
        "autodev status should count accepted task as passed");
    Expect(accepted_status.output.find("Progress:") != std::string::npos,
        "autodev status should display progress");
    Expect(accepted_status.output.find("overall:      90%") != std::string::npos,
        "autodev status should compute final review phase progress");
    Expect(accepted_status.output.find("acceptance:   1/1") != std::string::npos,
        "autodev status should show acceptance progress");
    const auto final_review = RunAgentos(workspace, {"autodev", "final-review", "job_id=" + executable_job_id});
    Expect(final_review.exit_code == 0,
        "autodev final-review should pass when accepted task facts and current diff are in scope");
    Expect(final_review.output.find("AutoDev final review checked") != std::string::npos,
        "autodev final-review should print heading");
    Expect(final_review.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev final-review should print final review id");
    Expect(final_review.output.find("passed:          true") != std::string::npos,
        "autodev final-review should report passed=true");
    Expect(final_review.output.find("job_status:      pr_ready") != std::string::npos,
        "autodev final-review should advance the job to pr_ready");
    Expect(std::filesystem::exists(executable_job_dir / "final_review.json"),
        "autodev final-review should write final_review.json under runtime store");
    const auto final_review_report_path = std::filesystem::path(executable_planned_path) / "docs" / "goal" / "FINAL_REVIEW.md";
    Expect(std::filesystem::exists(final_review_report_path),
        "autodev final-review should write FINAL_REVIEW.md under job worktree");
    const auto final_review_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + executable_job_id});
    Expect(final_review_status.exit_code == 0,
        "autodev status should read job after final review");
    Expect(final_review_status.output.find("Status: pr_ready") != std::string::npos,
        "autodev status should show pr_ready after final review");
    Expect(final_review_status.output.find("Next action:\n  none") != std::string::npos,
        "autodev status should show no next action after pr_ready");
    const auto final_reviews = RunAgentos(workspace, {"autodev", "final-reviews", "job_id=" + executable_job_id});
    Expect(final_reviews.exit_code == 0,
        "autodev final-reviews should list final review records");
    Expect(final_reviews.output.find("AutoDev final reviews") != std::string::npos,
        "autodev final-reviews should print heading");
    Expect(final_reviews.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev final-reviews should include final-review-001");
    Expect(final_reviews.output.find("passed:          true") != std::string::npos,
        "autodev final-reviews should show passed final review");
    const auto summary_result = RunAgentos(workspace, {"autodev", "summary", "job_id=" + executable_job_id});
    Expect(summary_result.exit_code == 0,
        "autodev summary should read job and runtime facts");
    Expect(summary_result.output.find("AutoDev summary") != std::string::npos,
        "autodev summary should print heading");
    Expect(summary_result.output.find("status:        pr_ready") != std::string::npos,
        "autodev summary should include current job status");
    Expect(summary_result.output.find("facts:         snapshots=1 verifications=1 diffs=1 acceptances=1 final_reviews=1 repairs=0") != std::string::npos,
        "autodev summary should include runtime fact counts");
    Expect(summary_result.output.find("overall:      100%") != std::string::npos,
        "autodev summary should show pr_ready progress as complete");
    Expect(summary_result.output.find("tasks:        1/1") != std::string::npos,
        "autodev summary should show task progress");
    Expect(summary_result.output.find("acceptance:   1/1") != std::string::npos,
        "autodev summary should show acceptance progress");
    Expect(summary_result.output.find("verification: verify-001 passed=true") != std::string::npos,
        "autodev summary should include latest verification fact per task");
    Expect(summary_result.output.find("diff_guard:   diff-001 passed=true") != std::string::npos,
        "autodev summary should include latest diff guard fact per task");
    Expect(summary_result.output.find("acceptance:   acceptance-001 passed=true") != std::string::npos,
        "autodev summary should include latest acceptance fact per task");
    Expect(summary_result.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev summary should include latest final review fact");
    const auto tasks_json_result = RunAgentos(workspace, {"autodev", "tasks", "job_id=" + executable_job_id, "format=json"});
    Expect(tasks_json_result.exit_code == 0,
        "autodev tasks format=json should succeed");
    Expect(tasks_json_result.output.find("\"job_id\": \"" + executable_job_id + "\"") != std::string::npos,
        "autodev tasks format=json should include job_id");
    Expect(tasks_json_result.output.find("\"tasks\": [") != std::string::npos &&
               tasks_json_result.output.find("\"task_id\": \"task-001\"") != std::string::npos,
        "autodev tasks format=json should include task ids");
    const auto verifications_json_result = RunAgentos(workspace, {"autodev", "verifications", "job_id=" + executable_job_id, "format=json"});
    Expect(verifications_json_result.exit_code == 0,
        "autodev verifications format=json should succeed");
    Expect(verifications_json_result.output.find("\"verifications\": [") != std::string::npos &&
               verifications_json_result.output.find("\"verification_id\": \"verify-001\"") != std::string::npos,
        "autodev verifications format=json should include verification records");
    const auto diffs_json_result = RunAgentos(workspace, {"autodev", "diffs", "job_id=" + executable_job_id, "format=json"});
    Expect(diffs_json_result.exit_code == 0,
        "autodev diffs format=json should succeed");
    Expect(diffs_json_result.output.find("\"diffs\": [") != std::string::npos &&
               diffs_json_result.output.find("\"diff_id\": \"diff-001\"") != std::string::npos,
        "autodev diffs format=json should include diff records");
    const auto acceptances_json_result = RunAgentos(workspace, {"autodev", "acceptances", "job_id=" + executable_job_id, "format=json"});
    Expect(acceptances_json_result.exit_code == 0,
        "autodev acceptances format=json should succeed");
    Expect(acceptances_json_result.output.find("\"acceptances\": [") != std::string::npos &&
               acceptances_json_result.output.find("\"acceptance_id\": \"acceptance-001\"") != std::string::npos,
        "autodev acceptances format=json should include acceptance records");
    const auto final_reviews_json_result = RunAgentos(workspace, {"autodev", "final-reviews", "job_id=" + executable_job_id, "format=json"});
    Expect(final_reviews_json_result.exit_code == 0,
        "autodev final-reviews format=json should succeed");
    Expect(final_reviews_json_result.output.find("\"final_reviews\": [") != std::string::npos &&
               final_reviews_json_result.output.find("\"final_review_id\": \"final-review-001\"") != std::string::npos,
        "autodev final-reviews format=json should include final review records");
    const auto summary_json_result = RunAgentos(workspace, {"autodev", "summary", "job_id=" + executable_job_id, "format=json"});
    Expect(summary_json_result.exit_code == 0,
        "autodev summary format=json should succeed");
    Expect(summary_json_result.output.find("\"job\": {") != std::string::npos &&
               summary_json_result.output.find("\"job_id\": \"" + executable_job_id + "\"") != std::string::npos,
        "autodev summary format=json should include job object");
    Expect(summary_json_result.output.find("\"progress\": {") != std::string::npos &&
               summary_json_result.output.find("\"overall_percent\": 100") != std::string::npos,
        "autodev summary format=json should include progress object");
    Expect(summary_json_result.output.find("\"fact_counts\": {") != std::string::npos &&
               summary_json_result.output.find("\"final_reviews\": 1") != std::string::npos,
        "autodev summary format=json should include fact counts");
    const auto pr_summary = RunAgentos(workspace, {"autodev", "pr-summary", "job_id=" + executable_job_id});
    Expect(pr_summary.exit_code == 0,
        "autodev pr-summary should succeed after final review makes the job pr_ready");
    Expect(pr_summary.output.find("AutoDev PR summary") != std::string::npos,
        "autodev pr-summary should print a handoff heading");
    Expect(pr_summary.output.find("- changed_files: README.md") != std::string::npos,
        "autodev pr-summary should include final review changed files");
    Expect(pr_summary.output.find("verification: verify-001 passed=true") != std::string::npos,
        "autodev pr-summary should include verification facts");
    Expect(pr_summary.output.find("diff_guard: diff-001 passed=true") != std::string::npos,
        "autodev pr-summary should include diff guard facts");
    Expect(pr_summary.output.find("acceptance: acceptance-001 passed=true") != std::string::npos,
        "autodev pr-summary should include acceptance facts");
    Expect(pr_summary.output.find("final_review: final-review-001 passed=true") != std::string::npos,
        "autodev pr-summary should include final review facts");
    Expect(pr_summary.output.find("command=true") != std::string::npos,
        "autodev pr-summary should include verification commands run");
    const auto pipeline_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Run one task through pipeline",
        "skill_pack_path=" + skill_pack.string()});
    Expect(pipeline_submit.exit_code == 0,
        "autodev submit should support a pipeline fixture job");
    const auto pipeline_job_id = ExtractLineValue(pipeline_submit.output, "job_id:");
    const auto pipeline_worktree = ExtractLineValue(pipeline_submit.output, "job_worktree_path:");
    const auto pipeline_path = pipeline_worktree.substr(0, pipeline_worktree.find(" "));
    const auto pipeline_job_dir = workspace / "runtime" / "autodev" / "jobs" / pipeline_job_id;
    Expect(RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + pipeline_job_id}).exit_code == 0,
        "autodev prepare-workspace should prepare pipeline fixture job");
    Expect(RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + pipeline_job_id}).exit_code == 0,
        "autodev load-skill-pack should load pipeline fixture job");
    Expect(RunAgentos(workspace, {"autodev", "generate-goal-docs", "job_id=" + pipeline_job_id}).exit_code == 0,
        "autodev generate-goal-docs should generate pipeline fixture docs");
    {
        std::ofstream spec(std::filesystem::path(pipeline_path) / "docs" / "goal" / "AUTODEV_SPEC.json",
            std::ios::binary | std::ios::trunc);
        spec
            << "{\n"
            << "  \"schema_version\": \"1.0.0\",\n"
            << "  \"generated_by\": \"agentos-cli-test\",\n"
            << "  \"generated_by_skill_pack\": \"maxenergy/skills\",\n"
            << "  \"agentos_min_version\": \"0.1.0\",\n"
            << "  \"created_at\": \"2026-05-06T00:00:00Z\",\n"
            << "  \"objective\": \"Run one task through pipeline\",\n"
            << "  \"mode\": \"feature\",\n"
            << "  \"source_of_truth\": [\"docs/goal/REQUIREMENTS.md\"],\n"
            << "  \"tasks\": [\n"
            << "    {\n"
            << "      \"task_id\": \"task-001\",\n"
            << "      \"title\": \"Pipeline update README\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"true\",\n"
            << "      \"acceptance\": [\"Pipeline can accept the task\"]\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
    }
    const auto pipeline_validate = RunAgentos(workspace, {"autodev", "validate-spec", "job_id=" + pipeline_job_id});
    Expect(pipeline_validate.exit_code == 0,
        "autodev validate-spec should validate pipeline fixture spec");
    const auto pipeline_hash = ExtractLineValue(pipeline_validate.output, "spec_hash:");
    Expect(RunAgentos(workspace, {
        "autodev",
        "approve-spec",
        "job_id=" + pipeline_job_id,
        "spec_hash=" + pipeline_hash}).exit_code == 0,
        "autodev approve-spec should approve pipeline fixture spec");
    const auto run_task = RunAgentos(workspace, {
        "autodev",
        "run-task",
        "job_id=" + pipeline_job_id,
        "codex_cli_command=" + codex_fixture.string()});
    Expect(run_task.exit_code == 0,
        "autodev run-task should execute and gate the next pending task");
    Expect(run_task.output.find("AutoDev single-task pipeline") != std::string::npos,
        "autodev run-task should print pipeline heading");
    Expect(run_task.output.find("AutoDev execution completed") != std::string::npos,
        "autodev run-task should include execution stage output");
    Expect(run_task.output.find("verification_passed: true") != std::string::npos,
        "autodev run-task should run verification");
    Expect(run_task.output.find("diff_passed:   true") != std::string::npos,
        "autodev run-task should run diff guard");
    Expect(run_task.output.find("acceptance_passed: true") != std::string::npos,
        "autodev run-task should run acceptance gate");
    Expect(run_task.output.find("pipeline_status: passed") != std::string::npos,
        "autodev run-task should report a passed pipeline");
    Expect(std::filesystem::exists(pipeline_job_dir / "snapshots.json"),
        "autodev run-task should record a snapshot");
    Expect(std::filesystem::exists(pipeline_job_dir / "turns.json"),
        "autodev run-task should record a turn");
    Expect(std::filesystem::exists(pipeline_job_dir / "verification.json"),
        "autodev run-task should record verification facts");
    Expect(std::filesystem::exists(pipeline_job_dir / "diffs.json"),
        "autodev run-task should record diff guard facts");
    Expect(std::filesystem::exists(pipeline_job_dir / "acceptance.json"),
        "autodev run-task should record acceptance facts");
    const auto pipeline_tasks = ReadTextFile(pipeline_job_dir / "tasks.json");
    Expect(pipeline_tasks.find("\"status\": \"passed\"") != std::string::npos,
        "autodev run-task should mark accepted task passed");
    const auto pipeline_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + pipeline_job_id});
    Expect(pipeline_status.exit_code == 0,
        "autodev status should read pipeline fixture job");
    Expect(pipeline_status.output.find("Status: running") != std::string::npos,
        "autodev run-task should not mark the job done");
    Expect(pipeline_status.output.find("Phase: final_review") != std::string::npos,
        "autodev run-task should advance all-passed jobs to final_review");
    {
        std::ofstream stale_lock(pipeline_job_dir / "job.lock", std::ios::binary | std::ios::trunc);
        stale_lock << "stale lock from crashed process\n";
    }
    std::filesystem::remove(pipeline_job_dir / "responses" / "turn-001.md");
    {
        auto turns_json = ReadTextFile(pipeline_job_dir / "turns.json");
        const auto status_pos = turns_json.find("\"status\": \"completed\"");
        Expect(status_pos != std::string::npos,
            "pipeline fixture should have a completed turn before crash recovery test mutation");
        if (status_pos != std::string::npos) {
            turns_json.replace(status_pos, std::string("\"status\": \"completed\"").size(), "\"status\": \"running\"");
            std::ofstream turns_out(pipeline_job_dir / "turns.json", std::ios::binary | std::ios::trunc);
            turns_out << turns_json;
        }
    }
    const auto crash_recovery = RunAgentos(workspace, {"autodev", "recover-crash", "job_id=" + pipeline_job_id});
    Expect(crash_recovery.exit_code != 0,
        "autodev recover-crash should return nonzero when recovery blocks the job");
    Expect(crash_recovery.output.find("AutoDev crash recovery checked") != std::string::npos,
        "autodev recover-crash should print a recovery heading");
    Expect(crash_recovery.output.find("blocked:             true") != std::string::npos,
        "autodev recover-crash should report blocked recovery for incomplete runtime facts");
    Expect(crash_recovery.output.find("stale_lock_removed:  true") != std::string::npos,
        "autodev recover-crash should remove stale job runtime locks");
    Expect(crash_recovery.output.find("marked in-flight turn failed") != std::string::npos,
        "autodev recover-crash should identify in-flight turns");
    Expect(crash_recovery.output.find("recreated missing response_artifact") != std::string::npos,
        "autodev recover-crash should identify missing response artifacts");
    Expect(!std::filesystem::exists(pipeline_job_dir / "job.lock"),
        "autodev recover-crash should remove stale job.lock");
    Expect(std::filesystem::exists(pipeline_job_dir / "responses" / "turn-001.md"),
        "autodev recover-crash should recreate missing response artifact placeholders");
    const auto recovered_turns = ReadTextFile(pipeline_job_dir / "turns.json");
    Expect(recovered_turns.find("\"status\": \"failed\"") != std::string::npos,
        "autodev recover-crash should mark in-flight turns failed");
    Expect(recovered_turns.find("crash_recovered_missing_response_artifact") != std::string::npos,
        "autodev recover-crash should annotate missing response artifact recovery");
    const auto crash_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + pipeline_job_id});
    Expect(crash_status.exit_code == 0,
        "autodev status should read crash-recovered jobs");
    Expect(crash_status.output.find("Status: blocked") != std::string::npos,
        "autodev recover-crash should block jobs with incomplete runtime facts");
    Expect(crash_status.output.find("AutoDev crash recovery found incomplete runtime facts") != std::string::npos,
        "autodev recover-crash should persist a blocker");
    const auto crash_events = RunAgentos(workspace, {"autodev", "events", "job_id=" + pipeline_job_id});
    Expect(crash_events.exit_code == 0,
        "autodev events should list crash recovery events");
    Expect(crash_events.output.find("autodev.crash_recovery.completed") != std::string::npos,
        "autodev recover-crash should append a crash recovery event");
    const auto run_job_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Run all tasks through job loop",
        "skill_pack_path=" + skill_pack.string(),
        "worktree_cleanup_policy=delete_on_done"});
    Expect(run_job_submit.exit_code == 0,
        "autodev submit should support a run-job fixture");
    const auto run_job_id = ExtractLineValue(run_job_submit.output, "job_id:");
    const auto run_job_worktree = ExtractLineValue(run_job_submit.output, "job_worktree_path:");
    const auto run_job_path = run_job_worktree.substr(0, run_job_worktree.find(" "));
    const auto run_job_dir = workspace / "runtime" / "autodev" / "jobs" / run_job_id;
    Expect(RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + run_job_id}).exit_code == 0,
        "autodev prepare-workspace should prepare run-job fixture");
    Expect(RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + run_job_id}).exit_code == 0,
        "autodev load-skill-pack should load run-job fixture");
    Expect(RunAgentos(workspace, {"autodev", "generate-goal-docs", "job_id=" + run_job_id}).exit_code == 0,
        "autodev generate-goal-docs should generate run-job fixture docs");
    {
        std::ofstream spec(std::filesystem::path(run_job_path) / "docs" / "goal" / "AUTODEV_SPEC.json",
            std::ios::binary | std::ios::trunc);
        spec
            << "{\n"
            << "  \"schema_version\": \"1.0.0\",\n"
            << "  \"generated_by\": \"agentos-cli-test\",\n"
            << "  \"generated_by_skill_pack\": \"maxenergy/skills\",\n"
            << "  \"agentos_min_version\": \"0.1.0\",\n"
            << "  \"created_at\": \"2026-05-06T00:00:00Z\",\n"
            << "  \"objective\": \"Run all tasks through job loop\",\n"
            << "  \"mode\": \"feature\",\n"
            << "  \"source_of_truth\": [\"docs/goal/REQUIREMENTS.md\"],\n"
            << "  \"tasks\": [\n"
            << "    {\n"
            << "      \"task_id\": \"task-001\",\n"
            << "      \"title\": \"First loop task\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"true\",\n"
            << "      \"acceptance\": [\"First loop task passes\"]\n"
            << "    },\n"
            << "    {\n"
            << "      \"task_id\": \"task-002\",\n"
            << "      \"title\": \"Second loop task\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"true\",\n"
            << "      \"acceptance\": [\"Second loop task passes\"]\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
    }
    const auto run_job_validate = RunAgentos(workspace, {"autodev", "validate-spec", "job_id=" + run_job_id});
    Expect(run_job_validate.exit_code == 0,
        "autodev validate-spec should validate run-job fixture spec");
    const auto run_job_hash = ExtractLineValue(run_job_validate.output, "spec_hash:");
    Expect(RunAgentos(workspace, {
        "autodev",
        "approve-spec",
        "job_id=" + run_job_id,
        "spec_hash=" + run_job_hash}).exit_code == 0,
        "autodev approve-spec should approve run-job fixture spec");
    const auto run_job = RunAgentos(workspace, {
        "autodev",
        "run-job",
        "job_id=" + run_job_id,
        "codex_cli_command=" + codex_fixture.string()});
    Expect(run_job.exit_code == 0,
        "autodev run-job should loop pending tasks to final_review");
    Expect(run_job.output.find("AutoDev job run loop") != std::string::npos,
        "autodev run-job should print run loop heading");
    Expect(run_job.output.find("iteration:     1") != std::string::npos,
        "autodev run-job should run the first task iteration");
    Expect(run_job.output.find("iteration:     2") != std::string::npos,
        "autodev run-job should run the second task iteration");
    Expect(run_job.output.find("run_job_status: ready_for_final_review") != std::string::npos,
        "autodev run-job should stop at final_review");
    Expect(run_job.output.find("Job was not marked done") != std::string::npos,
        "autodev run-job should state that it does not complete the job");
    const auto run_job_tasks = ReadTextFile(run_job_dir / "tasks.json");
    Expect(run_job_tasks.find("\"task_id\": \"task-001\"") != std::string::npos &&
               run_job_tasks.find("\"task_id\": \"task-002\"") != std::string::npos,
        "autodev run-job should keep both runtime tasks");
    Expect(run_job_tasks.find("\"status\": \"pending\"") == std::string::npos,
        "autodev run-job should accept all pending tasks");
    const auto run_job_turns = RunAgentos(workspace, {"autodev", "turns", "job_id=" + run_job_id});
    Expect(run_job_turns.exit_code == 0,
        "autodev turns should list run-job turns");
    Expect(run_job_turns.output.find("turn_id:           turn-001") != std::string::npos &&
               run_job_turns.output.find("turn_id:           turn-002") != std::string::npos,
        "autodev run-job should record one turn per task");
    const auto run_job_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + run_job_id});
    Expect(run_job_status.exit_code == 0,
        "autodev status should read run-job fixture");
    Expect(run_job_status.output.find("Status: running") != std::string::npos,
        "autodev run-job should leave the job running");
    Expect(run_job_status.output.find("Phase: final_review") != std::string::npos,
        "autodev run-job should leave the job at final_review");
    const auto run_job_final_review = RunAgentos(workspace, {"autodev", "final-review", "job_id=" + run_job_id});
    Expect(run_job_final_review.exit_code == 0,
        "autodev final-review should pass for the run-job fixture");
    const auto run_job_complete = RunAgentos(workspace, {"autodev", "mark-done", "job_id=" + run_job_id});
    Expect(run_job_complete.exit_code == 0,
        "autodev mark-done should complete the delete_on_done fixture");
    Expect(run_job_complete.output.find("cleanup_policy:  delete_on_done") != std::string::npos,
        "autodev mark-done should report delete_on_done cleanup policy");
    Expect(run_job_complete.output.find("isolation_status: cleaned") != std::string::npos,
        "delete_on_done should clean the worktree during completion");
    Expect(!std::filesystem::exists(run_job_path),
        "delete_on_done should remove the job worktree after mark-done");
    Expect(std::filesystem::exists(run_job_dir / "job.json"),
        "delete_on_done should preserve runtime facts");
    const auto run_job_cleanup_events = RunAgentos(workspace, {"autodev", "events", "job_id=" + run_job_id});
    Expect(run_job_cleanup_events.exit_code == 0,
        "autodev events should read delete_on_done fixture events");
    Expect(run_job_cleanup_events.output.find("autodev.worktree.cleaned") != std::string::npos,
        "delete_on_done should append a worktree cleanup event");
    const auto interrupt_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Cancel running Codex CLI execution",
        "skill_pack_path=" + skill_pack.string(),
        "worktree_cleanup_policy=keep_always"});
    Expect(interrupt_submit.exit_code == 0,
        "autodev submit should support an interrupt fixture job");
    const auto interrupt_job_id = ExtractLineValue(interrupt_submit.output, "job_id:");
    const auto interrupt_worktree = ExtractLineValue(interrupt_submit.output, "job_worktree_path:");
    const auto interrupt_path = interrupt_worktree.substr(0, interrupt_worktree.find(" "));
    const auto interrupt_job_dir = workspace / "runtime" / "autodev" / "jobs" / interrupt_job_id;
    Expect(RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + interrupt_job_id}).exit_code == 0,
        "autodev prepare-workspace should prepare interrupt fixture");
    Expect(RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + interrupt_job_id}).exit_code == 0,
        "autodev load-skill-pack should load interrupt fixture");
    Expect(RunAgentos(workspace, {"autodev", "generate-goal-docs", "job_id=" + interrupt_job_id}).exit_code == 0,
        "autodev generate-goal-docs should generate interrupt fixture docs");
    {
        std::ofstream spec(std::filesystem::path(interrupt_path) / "docs" / "goal" / "AUTODEV_SPEC.json",
            std::ios::binary | std::ios::trunc);
        spec
            << "{\n"
            << "  \"schema_version\": \"1.0.0\",\n"
            << "  \"generated_by\": \"agentos-cli-test\",\n"
            << "  \"generated_by_skill_pack\": \"maxenergy/skills\",\n"
            << "  \"agentos_min_version\": \"0.1.0\",\n"
            << "  \"created_at\": \"2026-05-06T00:00:00Z\",\n"
            << "  \"objective\": \"Cancel running Codex CLI execution\",\n"
            << "  \"mode\": \"feature\",\n"
            << "  \"source_of_truth\": [\"docs/goal/REQUIREMENTS.md\"],\n"
            << "  \"tasks\": [\n"
            << "    {\n"
            << "      \"task_id\": \"task-001\",\n"
            << "      \"title\": \"Long running execution\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"true\",\n"
            << "      \"acceptance\": [\"Long execution can be cancelled\"]\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
    }
    const auto interrupt_validate = RunAgentos(workspace, {"autodev", "validate-spec", "job_id=" + interrupt_job_id});
    Expect(interrupt_validate.exit_code == 0,
        "autodev validate-spec should validate interrupt fixture spec");
    const auto interrupt_hash = ExtractLineValue(interrupt_validate.output, "spec_hash:");
    Expect(RunAgentos(workspace, {
        "autodev",
        "approve-spec",
        "job_id=" + interrupt_job_id,
        "spec_hash=" + interrupt_hash}).exit_code == 0,
        "autodev approve-spec should approve interrupt fixture spec");
    const auto interrupt_marker = workspace / "autodev-interrupt-started.txt";
    const auto long_codex_fixture = WriteLongAutoDevCodexCliFixture(workspace / "bin", interrupt_marker);
    auto running_task = std::async(std::launch::async, [&]() {
        return RunAgentos(workspace, {
            "autodev",
            "run-task",
            "job_id=" + interrupt_job_id,
            "codex_cli_command=" + long_codex_fixture.string()});
    });
    for (int i = 0; i < 80 && !std::filesystem::exists(interrupt_marker); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    Expect(std::filesystem::exists(interrupt_marker),
        "long Codex fixture should start before cancellation is requested");
    const auto cancel_interrupt = RunAgentos(workspace, {"autodev", "cancel", "job_id=" + interrupt_job_id});
    Expect(cancel_interrupt.exit_code == 0,
        "autodev cancel should update runtime while Codex CLI execution is running");
    const auto interrupted_task = running_task.get();
    Expect(interrupted_task.exit_code != 0,
        "running autodev run-task should stop after job cancellation");
    Expect(interrupted_task.output.find("stopped_stage: execute") != std::string::npos,
        "running autodev run-task should stop at the execute stage after cancellation");
    const auto interrupt_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + interrupt_job_id});
    Expect(interrupt_status.exit_code == 0,
        "autodev status should read cancelled interrupt fixture");
    Expect(interrupt_status.output.find("Status: cancelled") != std::string::npos,
        "autodev cancel should leave interrupted job cancelled");
    const auto keep_always_cleanup = RunAgentos(workspace, {"autodev", "cleanup-worktree", "job_id=" + interrupt_job_id});
    Expect(keep_always_cleanup.exit_code != 0,
        "autodev cleanup-worktree should reject keep_always jobs");
    Expect(keep_always_cleanup.output.find("keep_always prevents cleanup") != std::string::npos,
        "autodev cleanup-worktree should explain keep_always policy");
    const auto interrupted_response = ReadTextFile(interrupt_job_dir / "responses" / "turn-001.md");
    Expect(interrupted_response.find("interrupted by autodev job status: cancelled") != std::string::npos,
        "interrupted execution should persist interruption reason in response artifact");
    Expect(ReadTextFile(std::filesystem::path(interrupt_path) / "README.md").find("long fixture completed") == std::string::npos,
        "cancelled Codex CLI process should not finish its worktree write");
    {
        std::ofstream blocked(std::filesystem::path(executable_planned_path) / "package.json",
            std::ios::binary | std::ios::trunc);
        blocked << "{}\n";
    }
    const auto diff_guard_fail = RunAgentos(workspace, {
        "autodev",
        "diff-guard",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(diff_guard_fail.exit_code != 0,
        "autodev diff-guard should return nonzero when blocked file changes exist");
    Expect(diff_guard_fail.output.find("passed:  false") != std::string::npos,
        "autodev diff-guard should report failed diff guard");
    Expect(diff_guard_fail.output.find("package.json") != std::string::npos,
        "autodev diff-guard should print violating file");
    const auto acceptance_gate_fail = RunAgentos(workspace, {
        "autodev",
        "acceptance-gate",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(acceptance_gate_fail.exit_code != 0,
        "autodev acceptance-gate should fail when latest diff guard failed");
    Expect(acceptance_gate_fail.output.find("passed:        false") != std::string::npos,
        "autodev acceptance-gate should report failed acceptance");
    Expect(acceptance_gate_fail.output.find("latest diff guard did not pass") != std::string::npos,
        "autodev acceptance-gate should explain failed diff guard dependency");
    const auto final_review_fail = RunAgentos(workspace, {"autodev", "final-review", "job_id=" + executable_job_id});
    Expect(final_review_fail.exit_code != 0,
        "autodev final-review should fail when current worktree diff violates task policy");
    Expect(final_review_fail.output.find("final_review_id: final-review-002") != std::string::npos,
        "autodev final-review should append a failed final review record after blocked diff");
    Expect(final_review_fail.output.find("passed:          false") != std::string::npos,
        "autodev final-review should report failed final review after blocked diff");
    Expect(final_review_fail.output.find("job_status:      running") != std::string::npos,
        "failed final review after stale pr_ready should move the job back to running");
    Expect(final_review_fail.output.find("job_phase:       final_review") != std::string::npos,
        "failed final review after stale pr_ready should move the job back to final_review");
    Expect(final_review_fail.output.find("package.json") != std::string::npos,
        "autodev final-review should print the blocked file violation");
    Expect(final_review_fail.output.find("current diff includes blocked files") != std::string::npos,
        "autodev final-review should explain blocked file violations");
    Expect(final_review_fail.output.find("current diff includes files outside allowed scope") != std::string::npos,
        "autodev final-review should explain outside allowed file violations");
    const auto final_reviews_after_fail = RunAgentos(workspace, {"autodev", "final-reviews", "job_id=" + executable_job_id});
    Expect(final_reviews_after_fail.exit_code == 0,
        "autodev final-reviews should list failed final review facts");
    Expect(final_reviews_after_fail.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev final-reviews should retain the earlier passed final review");
    Expect(final_reviews_after_fail.output.find("final_review_id: final-review-002") != std::string::npos,
        "autodev final-reviews should include failed final-review-002");
    Expect(final_reviews_after_fail.output.find("current diff includes blocked files") != std::string::npos,
        "autodev final-reviews should show failed final review reasons");
    const auto stale_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + executable_job_id});
    Expect(stale_status.exit_code == 0,
        "autodev status should read job after failed stale final review");
    Expect(stale_status.output.find("Status: running") != std::string::npos,
        "autodev status should not leave stale failed final review jobs at pr_ready");
    Expect(stale_status.output.find("Phase: final_review") != std::string::npos,
        "autodev status should show final_review after stale pr_ready is invalidated");
    Expect(stale_status.output.find("agentos autodev final-review job_id=" + executable_job_id) != std::string::npos,
        "autodev status should point stale failed final review jobs back to final_review");
    const auto failed_summary = RunAgentos(workspace, {"autodev", "summary", "job_id=" + executable_job_id});
    Expect(failed_summary.exit_code == 0,
        "autodev summary should still read facts after failed gates");
    Expect(failed_summary.output.find("status:        running") != std::string::npos,
        "autodev summary should show stale failed final review jobs as running");
    Expect(failed_summary.output.find("phase:         final_review") != std::string::npos,
        "autodev summary should show stale failed final review jobs in final_review phase");
    Expect(failed_summary.output.find("facts:         snapshots=1 verifications=1 diffs=2 acceptances=2 final_reviews=2 repairs=2") != std::string::npos,
        "autodev summary should count failed gate facts");
    Expect(failed_summary.output.find("diff_guard:   diff-002 passed=false") != std::string::npos,
        "autodev summary should show latest failed diff guard");
    Expect(failed_summary.output.find("acceptance:   acceptance-002 passed=false") != std::string::npos,
        "autodev summary should show latest failed acceptance");
    Expect(failed_summary.output.find("diff_blocked_file_violations: package.json") != std::string::npos,
        "autodev summary should show blocked file violations from latest diff");
    Expect(failed_summary.output.find("acceptance_reasons: latest diff guard did not pass") != std::string::npos,
        "autodev summary should show latest acceptance failure reasons");
    Expect(failed_summary.output.find("final_review_id: final-review-002") != std::string::npos,
        "autodev summary should show latest failed final review");
    Expect(failed_summary.output.find("  passed:          false") != std::string::npos,
        "autodev summary should show latest final review failure state");
    Expect(failed_summary.output.find("current diff includes blocked files") != std::string::npos,
        "autodev summary should show latest final review reasons");
    Expect(failed_summary.output.find("repair_id:   repair-002") != std::string::npos,
        "autodev summary should show latest repair-needed fact");
    Expect(failed_summary.output.find("next_action: repair_task") != std::string::npos,
        "autodev summary should show next repair action");
    Expect(failed_summary.output.find("retry:       2/3") != std::string::npos,
        "autodev summary should show latest repair retry counters");
    const auto repairs_after_failed_gates = RunAgentos(workspace, {"autodev", "repairs", "job_id=" + executable_job_id});
    Expect(repairs_after_failed_gates.exit_code == 0,
        "autodev repairs should list repair-needed facts");
    Expect(repairs_after_failed_gates.output.find("AutoDev repairs") != std::string::npos,
        "autodev repairs should print heading");
    Expect(repairs_after_failed_gates.output.find("repair_id:   repair-001") != std::string::npos,
        "autodev repairs should include diff guard repair fact");
    Expect(repairs_after_failed_gates.output.find("source:      diff_guard diff-002") != std::string::npos,
        "autodev repairs should link diff guard repair source");
    Expect(repairs_after_failed_gates.output.find("source:      acceptance_gate acceptance-002") != std::string::npos,
        "autodev repairs should link acceptance repair source");
    Expect(repairs_after_failed_gates.output.find("retry:       1/3") != std::string::npos,
        "autodev repairs should show first repair retry counter");
    Expect(repairs_after_failed_gates.output.find("retry:       2/3") != std::string::npos,
        "autodev repairs should show second repair retry counter");
    Expect(repairs_after_failed_gates.output.find("retry_limit_exceeded: false") != std::string::npos,
        "autodev repairs should show retry limit status");
    Expect(repairs_after_failed_gates.output.find("prompt_artifact:") != std::string::npos,
        "autodev repairs should show repair prompt artifact path");
    Expect(std::filesystem::exists(executable_job_dir / "repairs" / "repair-001.prompt.md"),
        "failed diff guard should write repair prompt artifact");
    const auto repair_prompt = ReadTextFile(executable_job_dir / "repairs" / "repair-001.prompt.md");
    Expect(repair_prompt.find("same thread/session") != std::string::npos,
        "repair prompt artifact should require same thread/session repair");
    Expect(repair_prompt.find("diff_guard") != std::string::npos,
        "repair prompt artifact should include failed runtime fact source");
    const auto repair_next = RunAgentos(workspace, {"autodev", "repair-next", "job_id=" + executable_job_id});
    Expect(repair_next.exit_code == 0,
        "autodev repair-next should select the latest actionable repair");
    Expect(repair_next.output.find("AutoDev repair task") != std::string::npos,
        "autodev repair-next should print repair task heading");
    Expect(repair_next.output.find("task_id:      task-001") != std::string::npos,
        "autodev repair-next should show the selected task");
    Expect(repair_next.output.find("repair_id:    repair-002") != std::string::npos,
        "autodev repair-next should select the latest repair-needed fact");
    Expect(repair_next.output.find("same thread/session") != std::string::npos,
        "autodev repair-next should include the same-thread repair prompt preview");
    Expect(repair_next.output.find("prompt_artifact:") != std::string::npos,
        "autodev repair-next should show the repair prompt artifact path");
    Expect(repair_next.output.find("execute-next-task job_id=" + executable_job_id) != std::string::npos,
        "autodev repair-next should show the execution entrypoint");
    Expect(repair_next.output.find("verify-task job_id=" + executable_job_id + " task_id=task-001") != std::string::npos,
        "autodev repair-next should show the same-task verification command");
    Expect(repair_next.output.find("diff-guard job_id=" + executable_job_id + " task_id=task-001") != std::string::npos,
        "autodev repair-next should show the same-task diff guard command");
    Expect(repair_next.output.find("acceptance-gate job_id=" + executable_job_id + " task_id=task-001") != std::string::npos,
        "autodev repair-next should show the same-task acceptance command");
    const auto repair_task = RunAgentos(workspace, {
        "autodev",
        "repair-task",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(repair_task.exit_code == 0,
        "autodev repair-task should select a repair for the requested task");
    Expect(repair_task.output.find("repair_id:    repair-002") != std::string::npos,
        "autodev repair-task should use the latest repair for the requested task");
    const auto diffs_result = RunAgentos(workspace, {"autodev", "diffs", "job_id=" + executable_job_id});
    Expect(diffs_result.exit_code == 0,
        "autodev diffs should list diff guard facts");
    Expect(diffs_result.output.find("AutoDev diffs") != std::string::npos,
        "autodev diffs should print heading");
    Expect(diffs_result.output.find("diff_id: diff-001") != std::string::npos,
        "autodev diffs should include first diff record");
    Expect(diffs_result.output.find("diff_id: diff-002") != std::string::npos,
        "autodev diffs should include second diff record");
    const auto rollback_soft = RunAgentos(workspace, {
        "autodev",
        "rollback-soft",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(rollback_soft.exit_code == 0,
        "autodev rollback-soft should safely restore tracked task files in the job worktree");
    Expect(rollback_soft.output.find("AutoDev soft rollback recorded") != std::string::npos,
        "autodev rollback-soft should print success heading");
    Expect(rollback_soft.output.find("rollback_id: rollback-001") != std::string::npos,
        "autodev rollback-soft should print rollback id");
    Expect(rollback_soft.output.find("status:      completed") != std::string::npos,
        "autodev rollback-soft should report completed status");
    Expect(rollback_soft.output.find("destructive: false") != std::string::npos,
        "autodev rollback-soft should report non-destructive rollback");
    Expect(rollback_soft.output.find("target_files: README.md") != std::string::npos,
        "autodev rollback-soft should target the tracked allowed file");
    Expect(ReadTextFile(std::filesystem::path(executable_planned_path) / "README.md").find("allowed cli change") == std::string::npos,
        "autodev rollback-soft should restore tracked allowed file content");
    Expect(std::filesystem::exists(std::filesystem::path(executable_planned_path) / "package.json"),
        "autodev rollback-soft should not clean untracked blocked files");
    const auto rollbacks_after_soft = RunAgentos(workspace, {"autodev", "rollbacks", "job_id=" + executable_job_id});
    Expect(rollbacks_after_soft.exit_code == 0,
        "autodev rollbacks should list soft rollback facts");
    Expect(rollbacks_after_soft.output.find("rollback_id: rollback-001") != std::string::npos,
        "autodev rollbacks should include rollback-001");
    Expect(rollbacks_after_soft.output.find("mode:        soft") != std::string::npos,
        "autodev rollbacks should include rollback mode");
    Expect(rollbacks_after_soft.output.find("target_files: README.md") != std::string::npos,
        "autodev rollbacks should include rollback target files");
    const auto rollback_hard_denied = RunAgentos(workspace, {
        "autodev",
        "rollback-hard",
        "job_id=" + executable_job_id,
        "task_id=task-001"});
    Expect(rollback_hard_denied.exit_code != 0,
        "autodev rollback-hard should fail without explicit approval");
    Expect(rollback_hard_denied.output.find("hard rollback requires approval=hard_rollback_approved") != std::string::npos,
        "autodev rollback-hard should explain the approval gate");
    Expect(rollback_hard_denied.output.find("destructive: true") != std::string::npos,
        "autodev rollback-hard denial should disclose destructive intent");
    Expect(rollback_hard_denied.output.find("executed:    false") != std::string::npos,
        "autodev rollback-hard denial should not execute rollback");
    const auto rollbacks_after_hard = RunAgentos(workspace, {"autodev", "rollbacks", "job_id=" + executable_job_id});
    Expect(rollbacks_after_hard.exit_code == 0,
        "autodev rollbacks should list denied hard rollback facts");
    Expect(rollbacks_after_hard.output.find("rollback_id: rollback-002") != std::string::npos,
        "autodev rollbacks should include denied hard rollback fact");
    Expect(rollbacks_after_hard.output.find("mode:        hard") != std::string::npos,
        "autodev rollbacks should show hard rollback mode");
    Expect(rollbacks_after_hard.output.find("status:      approval_required") != std::string::npos,
        "autodev rollbacks should show hard rollback approval gate status");

    const auto failing_verify_submit = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + target.string(),
        "objective=Expose failed verification in summary",
        "skill_pack_path=" + skill_pack.string()});
    Expect(failing_verify_submit.exit_code == 0,
        "autodev submit should support a failing verification fixture job");
    const auto failing_verify_job_id = ExtractLineValue(failing_verify_submit.output, "job_id:");
    const auto failing_verify_worktree = ExtractLineValue(failing_verify_submit.output, "job_worktree_path:");
    const auto failing_verify_path = failing_verify_worktree.substr(0, failing_verify_worktree.find(" "));
    const auto failing_verify_job_dir = workspace / "runtime" / "autodev" / "jobs" / failing_verify_job_id;

    Expect(RunAgentos(workspace, {"autodev", "prepare-workspace", "job_id=" + failing_verify_job_id}).exit_code == 0,
        "autodev prepare-workspace should prepare failing verification fixture job");
    Expect(RunAgentos(workspace, {"autodev", "load-skill-pack", "job_id=" + failing_verify_job_id}).exit_code == 0,
        "autodev load-skill-pack should load failing verification fixture job");
    Expect(RunAgentos(workspace, {"autodev", "generate-goal-docs", "job_id=" + failing_verify_job_id}).exit_code == 0,
        "autodev generate-goal-docs should generate failing verification fixture docs");
    {
        std::ofstream spec(std::filesystem::path(failing_verify_path) / "docs" / "goal" / "AUTODEV_SPEC.json",
            std::ios::binary | std::ios::trunc);
        spec
            << "{\n"
            << "  \"schema_version\": \"1.0.0\",\n"
            << "  \"generated_by\": \"agentos-cli-test\",\n"
            << "  \"generated_by_skill_pack\": \"maxenergy/skills\",\n"
            << "  \"agentos_min_version\": \"0.1.0\",\n"
            << "  \"created_at\": \"2026-05-06T00:00:00Z\",\n"
            << "  \"objective\": \"Expose failed verification in summary\",\n"
            << "  \"mode\": \"feature\",\n"
            << "  \"source_of_truth\": [\"docs/goal/REQUIREMENTS.md\"],\n"
            << "  \"tasks\": [\n"
            << "    {\n"
            << "      \"task_id\": \"task-001\",\n"
            << "      \"title\": \"Fail verification deliberately\",\n"
            << "      \"allowed_files\": [\"README.md\"],\n"
            << "      \"blocked_files\": [\"package.json\"],\n"
            << "      \"verify_command\": \"cmake -E false\",\n"
            << "      \"acceptance\": [\"Verification failure is visible\"]\n"
            << "    }\n"
            << "  ]\n"
            << "}\n";
    }
    const auto failing_verify_validate = RunAgentos(workspace, {"autodev", "validate-spec", "job_id=" + failing_verify_job_id});
    Expect(failing_verify_validate.exit_code == 0,
        "autodev validate-spec should validate failing verification fixture spec");
    const auto failing_verify_hash = ExtractLineValue(failing_verify_validate.output, "spec_hash:");
    Expect(RunAgentos(workspace, {
        "autodev",
        "approve-spec",
        "job_id=" + failing_verify_job_id,
        "spec_hash=" + failing_verify_hash}).exit_code == 0,
        "autodev approve-spec should approve failing verification fixture spec");
    const auto app_server_execute_next = RunAgentos(workspace, {
        "autodev",
        "execute-next-task",
        "job_id=" + failing_verify_job_id,
        "execution_adapter=codex_app_server"});
    Expect(app_server_execute_next.exit_code != 0,
        "autodev execute-next-task should fail closed for the Codex app-server skeleton");
    Expect(app_server_execute_next.output.find("adapter_kind:                codex_app_server") != std::string::npos,
        "autodev execute-next-task should expose the Codex app-server adapter kind");
    Expect(app_server_execute_next.output.find("continuity_mode:             persistent_thread") != std::string::npos,
        "autodev execute-next-task should expose persistent thread continuity");
    Expect(app_server_execute_next.output.find("event_stream_mode:           native_app_server") != std::string::npos,
        "autodev execute-next-task should expose native app-server event mode");
    Expect(app_server_execute_next.output.find("supports_persistent_session: true") != std::string::npos,
        "autodev execute-next-task should expose app-server persistent session support");
    Expect(app_server_execute_next.output.find("supports_same_thread_repair: true") != std::string::npos,
        "autodev execute-next-task should expose app-server same-thread repair support");
    Expect(app_server_execute_next.output.find("healthy:                     false") != std::string::npos,
        "autodev execute-next-task should keep the app-server skeleton fail-closed");
    Expect(app_server_execute_next.output.find("Codex app-server AutoDev execution is not implemented") != std::string::npos,
        "autodev execute-next-task should explain the app-server skeleton blocker");
    const auto failing_verify = RunAgentos(workspace, {
        "autodev",
        "verify-task",
        "job_id=" + failing_verify_job_id,
        "task_id=task-001"});
    Expect(failing_verify.exit_code != 0,
        "autodev verify-task should return nonzero when verify_command fails");
    Expect(failing_verify.output.find("passed:          false") != std::string::npos,
        "autodev verify-task should report failed verification");
    Expect(RunAgentos(workspace, {
        "autodev",
        "diff-guard",
        "job_id=" + failing_verify_job_id,
        "task_id=task-001"}).exit_code == 0,
        "autodev diff-guard should pass when failing verification fixture has no code diff violations");
    const auto failing_acceptance = RunAgentos(workspace, {
        "autodev",
        "acceptance-gate",
        "job_id=" + failing_verify_job_id,
        "task_id=task-001"});
    Expect(failing_acceptance.exit_code != 0,
        "autodev acceptance-gate should fail when latest verification failed");
    Expect(failing_acceptance.output.find("latest verification did not pass") != std::string::npos,
        "autodev acceptance-gate should explain failed verification dependency");
    const auto failing_final_review = RunAgentos(workspace, {"autodev", "final-review", "job_id=" + failing_verify_job_id});
    Expect(failing_final_review.exit_code != 0,
        "autodev final-review should fail when the task was not accepted");
    Expect(failing_final_review.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev final-review should write a failed final review record for unaccepted task");
    Expect(failing_final_review.output.find("passed:          false") != std::string::npos,
        "autodev final-review should report failed final review for unaccepted task");
    Expect(failing_final_review.output.find("job_status:      running") != std::string::npos,
        "autodev final-review should not advance unaccepted jobs to pr_ready");
    Expect(failing_final_review.output.find("job_phase:       codex_execution") != std::string::npos,
        "autodev final-review should keep unaccepted jobs in codex_execution");
    Expect(failing_final_review.output.find("task is not passed: task-001") != std::string::npos,
        "autodev final-review should explain unpassed task status");
    Expect(failing_final_review.output.find("task has no passed acceptance fact: task-001") != std::string::npos,
        "autodev final-review should explain missing passed acceptance fact");
    const auto failing_final_reviews = RunAgentos(workspace, {"autodev", "final-reviews", "job_id=" + failing_verify_job_id});
    Expect(failing_final_reviews.exit_code == 0,
        "autodev final-reviews should list failed unaccepted-task final review");
    Expect(failing_final_reviews.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev final-reviews should include failed unaccepted-task final review");
    Expect(failing_final_reviews.output.find("task is not passed: task-001") != std::string::npos,
        "autodev final-reviews should show unaccepted-task final review reasons");
    const auto failing_final_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + failing_verify_job_id});
    Expect(failing_final_status.exit_code == 0,
        "autodev status should read unaccepted job after failed final review");
    Expect(failing_final_status.output.find("Status: running") != std::string::npos,
        "failed final review for unaccepted task should leave job running");
    Expect(failing_final_status.output.find("Phase: codex_execution") != std::string::npos,
        "failed final review for unaccepted task should not advance phase to pr_ready");
    const auto failing_complete = RunAgentos(workspace, {"autodev", "mark-done", "job_id=" + failing_verify_job_id});
    Expect(failing_complete.exit_code != 0,
        "autodev mark-done should fail when job is not pr_ready");
    Expect(failing_complete.output.find("job is not pr_ready") != std::string::npos,
        "autodev mark-done should explain the pr_ready completion gate");
    const auto failing_verify_summary = RunAgentos(workspace, {"autodev", "summary", "job_id=" + failing_verify_job_id});
    Expect(failing_verify_summary.exit_code == 0,
        "autodev summary should read failed verification facts");
    Expect(failing_verify_summary.output.find("facts:         snapshots=1 verifications=1 diffs=1 acceptances=1 final_reviews=1 repairs=2") != std::string::npos,
        "autodev summary should count failed verification fixture facts");
    Expect(failing_verify_summary.output.find("overall:      30%") != std::string::npos,
        "autodev summary should show codex_execution phase-weight progress");
    Expect(failing_verify_summary.output.find("acceptance:   0/1") != std::string::npos,
        "autodev summary should show failed acceptance progress");
    Expect(failing_verify_summary.output.find("verification: verify-001 passed=false") != std::string::npos,
        "autodev summary should show latest failed verification");
    Expect(failing_verify_summary.output.find("verification_exit_code: 1") != std::string::npos,
        "autodev summary should show failed verification exit code");
    Expect(failing_verify_summary.output.find("acceptance:   acceptance-001 passed=false") != std::string::npos,
        "autodev summary should show acceptance failed after verification failure");
    Expect(failing_verify_summary.output.find("acceptance_reasons: latest verification did not pass") != std::string::npos,
        "autodev summary should show verification failure acceptance reason");
    Expect(failing_verify_summary.output.find("final_review_id: final-review-001") != std::string::npos,
        "autodev summary should show failed unaccepted-task final review");
    Expect(failing_verify_summary.output.find("task is not passed: task-001") != std::string::npos,
        "autodev summary should show failed final review task reason");
    Expect(failing_verify_summary.output.find("repair_id:   repair-002") != std::string::npos,
        "autodev summary should show latest repair fact for failed verification fixture");
    Expect(failing_verify_summary.output.find("source:      acceptance_gate acceptance-001") != std::string::npos,
        "autodev summary should show repair source for failed verification fixture");
    const auto failing_repairs = RunAgentos(workspace, {"autodev", "repairs", "job_id=" + failing_verify_job_id});
    Expect(failing_repairs.exit_code == 0,
        "autodev repairs should list failed verification repair facts");
    Expect(failing_repairs.output.find("source:      verification verify-001") != std::string::npos,
        "autodev repairs should include verification repair source");
    Expect(failing_repairs.output.find("source:      acceptance_gate acceptance-001") != std::string::npos,
        "autodev repairs should include acceptance repair source");
    Expect(failing_repairs.output.find("prompt_artifact:") != std::string::npos,
        "autodev repairs should show failed verification repair prompt artifact");
    const auto pause_failed_job = RunAgentos(workspace, {"autodev", "pause", "job_id=" + failing_verify_job_id});
    Expect(pause_failed_job.exit_code == 0,
        "autodev pause should update job status without stopping a process");
    Expect(pause_failed_job.output.find("AutoDev job paused") != std::string::npos,
        "autodev pause should print paused heading");
    Expect(pause_failed_job.output.find("status:      paused") != std::string::npos,
        "autodev pause should report paused status");
    Expect(pause_failed_job.output.find("next_action: resume") != std::string::npos,
        "autodev pause should make resume the next action");
    Expect(pause_failed_job.output.find("No Codex process was interrupted") != std::string::npos,
        "autodev pause should state that it does not interrupt Codex");
    const auto resume_failed_job = RunAgentos(workspace, {"autodev", "resume", "job_id=" + failing_verify_job_id});
    Expect(resume_failed_job.exit_code == 0,
        "autodev resume should restore a paused job to running");
    Expect(resume_failed_job.output.find("AutoDev job resumed") != std::string::npos,
        "autodev resume should print resumed heading");
    Expect(resume_failed_job.output.find("status:      running") != std::string::npos,
        "autodev resume should report running status");
    Expect(resume_failed_job.output.find("next_action: execute_next_task") != std::string::npos,
        "autodev resume should restore codex_execution next action");
    const auto cancel_failed_job = RunAgentos(workspace, {"autodev", "cancel", "job_id=" + failing_verify_job_id});
    Expect(cancel_failed_job.exit_code == 0,
        "autodev cancel should update job status without killing a process");
    Expect(cancel_failed_job.output.find("AutoDev job cancelled") != std::string::npos,
        "autodev cancel should print cancelled heading");
    Expect(cancel_failed_job.output.find("status:      cancelled") != std::string::npos,
        "autodev cancel should report cancelled status");
    Expect(cancel_failed_job.output.find("phase:       cancelled") != std::string::npos,
        "autodev cancel should report cancelled phase");
    Expect(cancel_failed_job.output.find("next_action: none") != std::string::npos,
        "autodev cancel should clear next action");
    const auto cleanup_cancelled_job = RunAgentos(workspace, {"autodev", "cleanup-worktree", "job_id=" + failing_verify_job_id});
    Expect(cleanup_cancelled_job.exit_code == 0,
        "autodev cleanup-worktree should clean cancelled job worktree");
    Expect(cleanup_cancelled_job.output.find("AutoDev worktree cleaned") != std::string::npos,
        "autodev cleanup-worktree should print heading");
    Expect(cleanup_cancelled_job.output.find("isolation_status:   cleaned") != std::string::npos,
        "autodev cleanup-worktree should mark isolation cleaned");
    Expect(cleanup_cancelled_job.output.find("removed:            true") != std::string::npos,
        "autodev cleanup-worktree should remove existing worktree");
    Expect(!std::filesystem::exists(failing_verify_path),
        "autodev cleanup-worktree should remove the job worktree path");
    Expect(std::filesystem::exists(failing_verify_job_dir / "job.json"),
        "autodev cleanup-worktree should preserve runtime facts");
    const auto cancelled_status = RunAgentos(workspace, {"autodev", "status", "job_id=" + failing_verify_job_id});
    Expect(cancelled_status.exit_code == 0,
        "autodev status should read cancelled job");
    Expect(cancelled_status.output.find("Status: cancelled") != std::string::npos,
        "autodev status should show cancelled status");
    const auto watch_cancelled_status = RunAgentos(workspace, {
        "autodev",
        "status",
        "job_id=" + failing_verify_job_id,
        "--watch",
        "iterations=1",
        "interval_ms=0"});
    Expect(watch_cancelled_status.exit_code == 0,
        "autodev status --watch should poll job status once when iterations=1");
    Expect(watch_cancelled_status.output.find("AutoDev watch") != std::string::npos,
        "autodev status --watch should print watch heading");
    Expect(watch_cancelled_status.output.find("status:      cancelled") != std::string::npos,
        "autodev status --watch should show current status");
    Expect(watch_cancelled_status.output.find("latest:      autodev.worktree.cleaned") != std::string::npos,
        "autodev status --watch should show the latest event");
    const auto watch_alias = RunAgentos(workspace, {
        "autodev",
        "watch",
        "job_id=" + failing_verify_job_id,
        "iterations=1",
        "interval_ms=0"});
    Expect(watch_alias.exit_code == 0,
        "autodev watch should alias status --watch");
    Expect(watch_alias.output.find("AutoDev watch") != std::string::npos,
        "autodev watch should print watch heading");
    const auto cancelled_events = RunAgentos(workspace, {"autodev", "events", "job_id=" + failing_verify_job_id});
    Expect(cancelled_events.exit_code == 0,
        "autodev events should read pause/resume/cancel events");
    Expect(cancelled_events.output.find("autodev.job.paused") != std::string::npos,
        "autodev pause should append a paused event");
    Expect(cancelled_events.output.find("autodev.job.resumed") != std::string::npos,
        "autodev resume should append a resumed event");
    Expect(cancelled_events.output.find("autodev.job.cancelled") != std::string::npos,
        "autodev cancel should append a cancelled event");
    Expect(cancelled_events.output.find("autodev.worktree.cleaned") != std::string::npos,
        "autodev cleanup-worktree should append a cleanup event");

    const auto executable_events = RunAgentos(workspace, {"autodev", "events", "job_id=" + executable_job_id});
    Expect(executable_events.exit_code == 0,
        "autodev events should read execution preflight audit event");
    Expect(executable_events.output.find("autodev.execution.completed") != std::string::npos,
        "autodev execute-next-task should append an execution completed audit event");
    Expect(executable_events.output.find("autodev.snapshot.recorded") != std::string::npos,
        "autodev execute-next-task should append a snapshot recorded event");
    Expect(executable_events.output.find("autodev.rollback.recorded") != std::string::npos,
        "autodev rollback-soft should append a rollback recorded event");
    Expect(executable_events.output.find("autodev.rollback.denied") != std::string::npos,
        "autodev rollback-hard should append a denied rollback event");
    Expect(executable_events.output.find("autodev.repair.needed") != std::string::npos,
        "failed gates should append repair needed events");
    Expect(executable_events.output.find("prompt_artifact") != std::string::npos,
        "repair needed event should link repair prompt artifact");
    Expect(executable_events.output.find("autodev.verification.completed") != std::string::npos,
        "autodev verify-task should append a verification completed event");
    Expect(executable_events.output.find("autodev.diff_guard.completed") != std::string::npos,
        "autodev diff-guard should append a diff guard completed event");
    Expect(executable_events.output.find("autodev.acceptance_gate.completed") != std::string::npos,
        "autodev acceptance-gate should append an acceptance completed event");
    Expect(executable_events.output.find("autodev.final_review.completed") != std::string::npos,
        "autodev final-review should append a final review completed event");
    const auto diff_events_json = RunAgentos(workspace, {
        "autodev",
        "events",
        "job_id=" + executable_job_id,
        "format=json",
        "type=autodev.diff_guard.completed"});
    Expect(diff_events_json.exit_code == 0,
        "autodev events format=json should support type filters");
    Expect(diff_events_json.output.find("\"type\": \"autodev.diff_guard.completed\"") != std::string::npos,
        "autodev events format=json should preserve event type");
    Expect(diff_events_json.output.find("\"events\": [") != std::string::npos,
        "autodev events format=json should include events array");
    Expect(diff_events_json.output.find("\"type\": \"autodev.verification.completed\"") == std::string::npos,
        "autodev events type filter should exclude other event types");
    const auto future_events_json = RunAgentos(workspace, {
        "autodev",
        "events",
        "job_id=" + executable_job_id,
        "format=json",
        "since=9999-01-01T00:00:00Z"});
    Expect(future_events_json.exit_code == 0,
        "autodev events format=json should support since filters");
    Expect(future_events_json.output.find("\"total\": 0") != std::string::npos,
        "autodev events since filter should return zero future events");
    const auto jobs_dashboard = RunAgentos(workspace, {"autodev", "jobs"});
    Expect(jobs_dashboard.exit_code == 0,
        "autodev jobs should list all AutoDev jobs");
    Expect(jobs_dashboard.output.find("AutoDev jobs") != std::string::npos,
        "autodev jobs should print dashboard heading");
    Expect(jobs_dashboard.output.find("job_id:       " + executable_job_id) != std::string::npos,
        "autodev jobs should include executable fixture job");
    Expect(jobs_dashboard.output.find("job_id:       " + failing_verify_job_id) != std::string::npos,
        "autodev jobs should include failing fixture job");
    Expect(jobs_dashboard.output.find("progress:") != std::string::npos,
        "autodev jobs should show progress");
    Expect(jobs_dashboard.output.find("next_action:") != std::string::npos,
        "autodev jobs should show next actions");
    Expect(jobs_dashboard.output.find("blocker:") != std::string::npos,
        "autodev jobs should show blockers when present");
    const auto jobs_json = RunAgentos(workspace, {"autodev", "list", "format=json"});
    Expect(jobs_json.exit_code == 0,
        "autodev list format=json should list all AutoDev jobs");
    Expect(jobs_json.output.find("\"jobs\": [") != std::string::npos,
        "autodev list format=json should include jobs array");
    Expect(jobs_json.output.find("\"job_id\": \"" + executable_job_id + "\"") != std::string::npos,
        "autodev list format=json should include job ids");
    Expect(jobs_json.output.find("\"overall_percent\"") != std::string::npos,
        "autodev list format=json should include progress");
    Expect(jobs_json.output.find("\"next_action\"") != std::string::npos,
        "autodev list format=json should include next actions");

    const auto invalid_status = RunAgentos(workspace, {"autodev", "status", "job_id=../bad"});
    Expect(invalid_status.exit_code != 0, "autodev status should reject invalid job id");
    Expect(invalid_status.output.find("invalid job_id") != std::string::npos,
        "autodev status should explain invalid job id");

    const auto missing_target = RunAgentos(workspace, {
        "autodev",
        "submit",
        "target_repo_path=" + (workspace / "missing").string(),
        "objective=Fix bug"});
    Expect(missing_target.exit_code != 0, "autodev submit should fail for missing target_repo_path");
    Expect(missing_target.output.find("target_repo_path does not exist") != std::string::npos,
        "autodev submit should explain missing target_repo_path");
}

void TestDiagnosticsCommand() {
    const auto workspace = FreshWorkspace("diagnostics");

    const auto text_result = RunAgentos(workspace, {"diagnostics"});
    Expect(text_result.exit_code == 0, "diagnostics command should succeed with default text format");
    Expect(text_result.output.find("[platform]") != std::string::npos,
        "diagnostics text output should include [platform] section header");
    Expect(text_result.output.find("[auth]") != std::string::npos,
        "diagnostics text output should include [auth] section header");
    Expect(text_result.output.find("[agents]") != std::string::npos,
        "diagnostics text output should include [agents] section header");
    Expect(text_result.output.find("[plugins]") != std::string::npos,
        "diagnostics text output should include [plugins] section header");
    Expect(text_result.output.find("[scheduler]") != std::string::npos,
        "diagnostics text output should include [scheduler] section header");
    Expect(text_result.output.find("[storage]") != std::string::npos,
        "diagnostics text output should include [storage] section header");
    Expect(text_result.output.find("[trust]") != std::string::npos,
        "diagnostics text output should include [trust] section header");
    Expect(text_result.output.find("local_planner") != std::string::npos,
        "diagnostics text output should list registered agents");

    const auto json_result = RunAgentos(workspace, {"diagnostics", "format=json"});
    Expect(json_result.exit_code == 0, "diagnostics command should succeed with format=json");
    Expect(!json_result.output.empty() && json_result.output.front() == '{',
        "diagnostics json output should start with '{'");
    const auto trimmed_end = json_result.output.find_last_not_of(" \t\r\n");
    Expect(trimmed_end != std::string::npos && json_result.output[trimmed_end] == '}',
        "diagnostics json output should end with '}'");
    Expect(json_result.output.find("\"platform\"") != std::string::npos,
        "diagnostics json output should include platform key");
    Expect(json_result.output.find("\"auth\"") != std::string::npos,
        "diagnostics json output should include auth key");
    Expect(json_result.output.find("\"agents\"") != std::string::npos,
        "diagnostics json output should include agents key");
    Expect(json_result.output.find("\"plugins\"") != std::string::npos,
        "diagnostics json output should include plugins key");
    Expect(json_result.output.find("\"scheduler\"") != std::string::npos,
        "diagnostics json output should include scheduler key");
    Expect(json_result.output.find("\"storage\"") != std::string::npos,
        "diagnostics json output should include storage key");
    Expect(json_result.output.find("\"trust\"") != std::string::npos,
        "diagnostics json output should include trust key");
}

}  // namespace

int main() {
    TestAgentsCommand();
    TestSkillsCommandShowsRepoAgentSkills();
    TestCliSpecsCommand();
    TestSpecNameConflictsAreAudited();
    TestPluginNameConflictsWithExternalCliSpec();
    TestPluginsCommand();
    TestMemoryAndStorageCommands();
    TestAutoDevCommands();
    TestTrustCommands();
    TestScheduleCommands();
    TestSubagentsCommand();
    TestAuthCommands();
    TestRunAuthProfileOverride();
    TestInteractiveFreeFormDispatch();
    TestInteractiveMainRouteActionLoop();
    TestInteractiveMainRouteActionValidationLoop();
    TestInteractiveMainRouteActionContextAfterClarification();
    TestDiagnosticsCommand();

    if (failures != 0) {
        std::cerr << failures << " cli integration test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_cli_integration_tests passed\n";
    return 0;
}
