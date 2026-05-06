#include "cli/autodev_commands.hpp"

#include "autodev/autodev_execution_adapter.hpp"
#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_state_store.hpp"

#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agentos {

namespace {

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument.rfind("--", 0) == 0) {
            argument = argument.substr(2);
        }
        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            if (!argument.empty()) {
                options[argument] = "true";
            }
            continue;
        }
        options[argument.substr(0, separator)] = argument.substr(separator + 1);
    }
    return options;
}

class AutoDevJobRuntimeLock {
public:
    explicit AutoDevJobRuntimeLock(std::filesystem::path lock_path)
        : lock_path_(std::move(lock_path)) {
        acquire();
    }

    AutoDevJobRuntimeLock(const AutoDevJobRuntimeLock&) = delete;
    AutoDevJobRuntimeLock& operator=(const AutoDevJobRuntimeLock&) = delete;

    ~AutoDevJobRuntimeLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            close(fd_);
            std::filesystem::remove(lock_path_);
        }
#endif
    }

private:
    void acquire() {
        std::filesystem::create_directories(lock_path_.parent_path());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (true) {
#ifdef _WIN32
            handle_ = CreateFileW(
                lock_path_.wstring().c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                throw std::runtime_error("could not create AutoDev job runtime lock with Windows error " +
                    std::to_string(error));
            }
#else
            fd_ = open(lock_path_.string().c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd_ >= 0) {
                return;
            }
            if (errno != EEXIST) {
                throw std::runtime_error("could not create AutoDev job runtime lock: " +
                    std::string(std::strerror(errno)));
            }
#endif
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("timed out waiting for AutoDev job runtime lock: " + lock_path_.string());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::filesystem::path lock_path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

bool AutoDevSubcommandNeedsJobRuntimeLock(const std::string& subcommand) {
    return subcommand == "prepare-workspace" || subcommand == "prepare_workspace" ||
        subcommand == "load-skill-pack" || subcommand == "load_skill_pack" ||
        subcommand == "generate-goal-docs" || subcommand == "generate_goal_docs" ||
        subcommand == "validate-spec" || subcommand == "validate_spec" ||
        subcommand == "approve-spec" || subcommand == "approve_spec" ||
        subcommand == "recover-blocked" || subcommand == "recover_blocked" ||
        subcommand == "snapshot-task" || subcommand == "snapshot_task" ||
        subcommand == "rollback-soft" || subcommand == "rollback_soft" ||
        subcommand == "rollback-hard" || subcommand == "rollback_hard" ||
        subcommand == "verify-task" || subcommand == "verify_task" ||
        subcommand == "diff-guard" || subcommand == "diff_guard" ||
        subcommand == "acceptance-gate" || subcommand == "acceptance_gate" ||
        subcommand == "final-review" || subcommand == "final_review" ||
        subcommand == "complete-job" || subcommand == "complete_job" ||
        subcommand == "mark-done" || subcommand == "mark_done" ||
        subcommand == "cleanup-worktree" || subcommand == "cleanup_worktree" ||
        subcommand == "execute-next-task" || subcommand == "execute_next_task" ||
        subcommand == "run-task" || subcommand == "run_task" ||
        subcommand == "pipeline-task" || subcommand == "pipeline_task" ||
        subcommand == "run-job" || subcommand == "run_job";
}

int ParseIntOption(
    const std::map<std::string, std::string>& options,
    const std::string& key,
    const int default_value) {
    const auto it = options.find(key);
    if (it == options.end() || it->second.empty()) {
        return default_value;
    }
    try {
        return std::stoi(it->second);
    } catch (...) {
        return default_value;
    }
}

struct AutoDevProgressView {
    int phase_weight = 0;
    int overall_percent = 0;
    std::size_t tasks_passed = 0;
    std::size_t tasks_total = 0;
    int acceptance_passed = 0;
    int acceptance_total = 0;
};

int PhaseWeight(const AutoDevJob& job) {
    if (job.status == "done" || job.phase == "done" || job.status == "pr_ready" || job.phase == "pr_ready") {
        return 100;
    }
    if (job.phase == "final_review") {
        return 90;
    }
    if (job.phase == "codex_execution" || job.phase == "repairing") {
        return 30;
    }
    if (job.phase == "spec_validation") {
        return 25;
    }
    if (job.phase == "goal_docs") {
        return 20;
    }
    if (job.phase == "skill_pack_loading") {
        return 10;
    }
    return 0;
}

AutoDevProgressView ComputeProgress(const AutoDevJob& job, const std::vector<AutoDevTask>& tasks) {
    AutoDevProgressView progress;
    progress.phase_weight = PhaseWeight(job);
    progress.tasks_total = tasks.size();
    for (const auto& task : tasks) {
        if (task.status == "passed") {
            ++progress.tasks_passed;
        }
        progress.acceptance_passed += task.acceptance_passed;
        progress.acceptance_total += task.acceptance_total;
    }
    if (progress.phase_weight == 100) {
        progress.overall_percent = 100;
        return progress;
    }
    if (progress.tasks_total > 0 &&
        (job.phase == "codex_execution" || job.phase == "repairing" || job.phase == "final_review")) {
        const auto task_percent = static_cast<int>((60 * progress.tasks_passed) / progress.tasks_total);
        progress.overall_percent = std::max(progress.phase_weight, 30 + task_percent);
        progress.overall_percent = std::min(progress.overall_percent, job.phase == "final_review" ? 90 : 89);
        return progress;
    }
    progress.overall_percent = progress.phase_weight;
    return progress;
}

void PrintProgress(const std::string& indent, const AutoDevProgressView& progress) {
    std::cout << indent << "overall:      " << progress.overall_percent << "%\n"
              << indent << "phase_weight: " << progress.phase_weight << "%\n"
              << indent << "tasks:        " << progress.tasks_passed << "/" << progress.tasks_total << '\n'
              << indent << "acceptance:   " << progress.acceptance_passed << "/" << progress.acceptance_total << '\n';
}

std::string ShellQuote(const std::string& value) {
#ifdef _WIN32
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
#else
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

struct ShellCommandResult {
    int exit_code = -1;
    std::string output;
    int duration_ms = 0;
};

[[maybe_unused]] ShellCommandResult RunShellCommand(const std::filesystem::path& cwd, const std::string& command) {
    const auto started = std::chrono::steady_clock::now();
    std::ostringstream shell;
#ifdef _WIN32
    shell << "cd /d " << ShellQuote(cwd.string()) << " && " << command << " 2>&1";
    FILE* pipe = _popen(shell.str().c_str(), "r");
#else
    shell << "cd " << ShellQuote(cwd.string()) << " && " << command << " 2>&1";
    FILE* pipe = popen(shell.str().c_str(), "r");
#endif
    if (!pipe) {
        return {.exit_code = -1, .output = "failed to start command", .duration_ms = 0};
    }
    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#ifdef _WIN32
    const int status = _pclose(pipe);
    const int exit_code = status;
#else
    const int status = pclose(pipe);
    int exit_code = status;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }
#endif
    const auto completed = std::chrono::steady_clock::now();
    const auto duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(completed - started).count());
    return {.exit_code = exit_code, .output = std::move(output), .duration_ms = duration_ms};
}

ShellCommandResult RunShellCommandWithAutoDevControl(
    const std::filesystem::path& cwd,
    const std::string& command,
    const std::filesystem::path& workspace,
    const std::string& job_id) {
#ifdef _WIN32
    (void)workspace;
    (void)job_id;
    return RunShellCommand(cwd, command);
#else
    const auto started = std::chrono::steady_clock::now();
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return {.exit_code = -1, .output = "failed to create process pipe", .duration_ms = 0};
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return {.exit_code = -1, .output = "failed to fork process", .duration_ms = 0};
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        const auto shell_command = "cd " + ShellQuote(cwd.string()) + " && " + command;
        execl("/bin/sh", "sh", "-c", shell_command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    setpgid(pid, pid);
    close(pipe_fds[1]);
    const int flags = fcntl(pipe_fds[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
    }

    AutoDevStateStore control_store(workspace);
    std::string output;
    int status = 0;
    bool exited = false;
    bool interrupt_sent = false;
    std::string interrupt_status;
    auto last_poll = std::chrono::steady_clock::now() - std::chrono::milliseconds(250);

    while (!exited) {
        char buffer[4096];
        while (true) {
            const ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer));
            if (n > 0) {
                output.append(buffer, static_cast<std::size_t>(n));
                continue;
            }
            break;
        }

        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            exited = true;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!interrupt_sent && now - last_poll >= std::chrono::milliseconds(100)) {
            last_poll = now;
            if (const auto job = control_store.load_job(job_id, nullptr); job.has_value()) {
                if (job->status == "paused" || job->status == "cancelled") {
                    interrupt_sent = true;
                    interrupt_status = job->status;
                    output += "\n[agentos] interrupted by autodev job status: " + interrupt_status + "\n";
                    kill(-pid, SIGTERM);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    char buffer[4096];
    while (true) {
        const ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        break;
    }
    close(pipe_fds[0]);

    const auto duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    int exit_code = status;
    if (interrupt_sent) {
        exit_code = interrupt_status == "cancelled" ? 130 : 131;
    } else if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }
    return {.exit_code = exit_code, .output = std::move(output), .duration_ms = duration_ms};
#endif
}

std::string BuildCodexCliPrompt(const AutoDevJob& job, const AutoDevTask& task) {
    std::ostringstream out;
    out << "You are executing an AgentOS AutoDev task.\n\n"
        << "Job ID: " << job.job_id << "\n"
        << "Spec revision: " << job.spec_revision.value_or("") << "\n"
        << "Spec hash: " << job.spec_hash.value_or("") << "\n"
        << "Worktree: " << job.job_worktree_path.string() << "\n\n"
        << "Task ID: " << task.task_id << "\n"
        << "Title: " << task.title << "\n";
    if (!task.allowed_files.empty()) {
        out << "Allowed files:\n";
        for (const auto& file : task.allowed_files) {
            out << "- " << file << "\n";
        }
    }
    if (!task.blocked_files.empty()) {
        out << "Blocked files:\n";
        for (const auto& file : task.blocked_files) {
            out << "- " << file << "\n";
        }
    }
    if (task.verify_command.has_value()) {
        out << "Verify command: " << *task.verify_command << "\n";
    }
    out << "\nInstructions:\n"
        << "- Modify only files needed for this task and stay within allowed_files.\n"
        << "- Do not edit AgentOS runtime files to change status.\n"
        << "- Leave task acceptance to AgentOS verification, diff guard, and acceptance gate.\n";
    return out.str();
}

bool WantsJson(const std::map<std::string, std::string>& options) {
    const auto it = options.find("format");
    return it != options.end() && it->second == "json";
}

template <typename T>
nlohmann::json RecordsJson(const std::vector<T>& records) {
    nlohmann::json json = nlohmann::json::array();
    for (const auto& record : records) {
        json.push_back(ToJson(record));
    }
    return json;
}

nlohmann::json ProgressJson(const AutoDevProgressView& progress) {
    return nlohmann::json{
        {"overall_percent", progress.overall_percent},
        {"phase_weight", progress.phase_weight},
        {"tasks_passed", progress.tasks_passed},
        {"tasks_total", progress.tasks_total},
        {"acceptance_passed", progress.acceptance_passed},
        {"acceptance_total", progress.acceptance_total},
    };
}

std::string AutoDevCommandForNextAction(const AutoDevJob& job) {
    const auto command = [&]() {
        if (job.next_action == "prepare_workspace" ||
            job.next_action == "commit_or_allow_dirty_target" ||
            job.next_action == "fix_workspace_or_use_git_worktree" ||
            job.next_action == "fix_workspace_or_use_in_place" ||
            job.next_action == "fix_workspace_path") {
            return std::string("prepare-workspace");
        }
        if (job.next_action == "load_skill_pack" ||
            job.next_action == "declare_skill_pack" ||
            job.next_action == "fix_skill_pack") {
            return std::string("load-skill-pack");
        }
        if (job.next_action == "generate_goal_docs") {
            return std::string("generate-goal-docs");
        }
        if (job.next_action == "validate_spec" || job.next_action == "fix_autodev_spec") {
            return std::string("validate-spec");
        }
        if (job.next_action == "approve_spec") {
            return std::string("approve-spec");
        }
        if (job.next_action == "final_review") {
            return std::string("final-review");
        }
        if (job.next_action == "complete_job") {
            return std::string("complete-job");
        }
        return job.next_action;
    }();
    if (command.empty() || command == "none") {
        return {};
    }
    std::string rendered = "agentos autodev " + command + " job_id=" + job.job_id;
    if (command == "approve-spec" && job.spec_hash.has_value()) {
        rendered += " spec_hash=" + *job.spec_hash;
    }
    return rendered;
}

std::string AutoDevRecoveryHint(const AutoDevJob& job) {
    if (job.status == "blocked") {
        const auto command = AutoDevCommandForNextAction(job);
        if (job.next_action == "commit_or_allow_dirty_target") {
            return "Commit/stash target repo changes, then run: agentos autodev recover-blocked job_id=" + job.job_id;
        }
        if (job.next_action == "declare_skill_pack") {
            return "Provide a skill pack path: agentos autodev recover-blocked job_id=" + job.job_id + " skill_pack_path=<path>";
        }
        if (job.next_action == "fix_skill_pack") {
            return "Fix the skill pack, then run: agentos autodev recover-blocked job_id=" + job.job_id;
        }
        if (job.next_action == "fix_autodev_spec") {
            return "Fix docs/goal/AUTODEV_SPEC.json in the job worktree, then run: agentos autodev recover-blocked job_id=" + job.job_id;
        }
        if (!command.empty()) {
            return "Run: agentos autodev recover-blocked job_id=" + job.job_id + " (or " + command + ")";
        }
        return "Inspect blocker and events, then rerun the corrected AutoDev gate.";
    }
    if (job.status == "awaiting_approval" && job.next_action == "approve_spec") {
        return "Approve the frozen spec explicitly: " + AutoDevCommandForNextAction(job);
    }
    return {};
}

void PrintJson(const nlohmann::json& json) {
    std::cout << json.dump(2) << '\n';
}

std::string ReadTextFileForCli(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) {
            *error = "unable to read file: " + path.string();
        }
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string PreviewText(const std::string& value, const std::size_t max_bytes = 4000) {
    if (value.size() <= max_bytes) {
        return value;
    }
    return value.substr(0, max_bytes) + "\n[truncated]\n";
}

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  agentos autodev submit target_repo_path=<path> objective=<text> [skill_pack_path=<path>] [isolation_mode=git_worktree|in_place] [allow_dirty_target=true|false]\n"
        << "  agentos autodev status job_id=<job_id>\n"
        << "  agentos autodev status job_id=<job_id> --watch [iterations=1] [interval_ms=1000]\n"
        << "  agentos autodev watch job_id=<job_id> [iterations=1] [interval_ms=1000]\n"
        << "  agentos autodev summary job_id=<job_id>\n"
        << "  agentos autodev prepare-workspace job_id=<job_id>\n"
        << "  agentos autodev load-skill-pack job_id=<job_id> [skill_pack_path=<path>]\n"
        << "  agentos autodev generate-goal-docs job_id=<job_id>\n"
        << "  agentos autodev validate-spec job_id=<job_id>\n"
        << "  agentos autodev approve-spec job_id=<job_id> spec_hash=<sha256> [spec_revision=rev-001]\n"
        << "  agentos autodev recover-blocked job_id=<job_id> [skill_pack_path=<path>]\n"
        << "  agentos autodev recover-crash job_id=<job_id>\n"
        << "  agentos autodev tasks job_id=<job_id>\n"
        << "  agentos autodev turns job_id=<job_id>\n"
        << "  agentos autodev snapshot-task job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev snapshots job_id=<job_id>\n"
        << "  agentos autodev rollback-soft job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev rollback-hard job_id=<job_id> task_id=<task_id> approval=hard_rollback_approved\n"
        << "  agentos autodev rollbacks job_id=<job_id>\n"
        << "  agentos autodev repairs job_id=<job_id>\n"
        << "  agentos autodev repair-next job_id=<job_id>\n"
        << "  agentos autodev repair-task job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev verify-task job_id=<job_id> task_id=<task_id> [related_turn_id=<turn_id>]\n"
        << "  agentos autodev verifications job_id=<job_id>\n"
        << "  agentos autodev diff-guard job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev diffs job_id=<job_id>\n"
        << "  agentos autodev acceptance-gate job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev acceptances job_id=<job_id>\n"
        << "  agentos autodev final-review job_id=<job_id>\n"
        << "  agentos autodev final-reviews job_id=<job_id>\n"
        << "  agentos autodev complete-job job_id=<job_id>\n"
        << "  agentos autodev pause job_id=<job_id>\n"
        << "  agentos autodev resume job_id=<job_id>\n"
        << "  agentos autodev cancel job_id=<job_id>\n"
        << "  agentos autodev cleanup-worktree job_id=<job_id>\n"
        << "  agentos autodev pr-summary job_id=<job_id>\n"
        << "  agentos autodev events job_id=<job_id>\n"
        << "  agentos autodev run-job job_id=<job_id> [execution_adapter=codex_cli|codex_app_server] [codex_cli_command=<command>] [app_server_url=<url>]\n"
        << "  agentos autodev run-task job_id=<job_id> [execution_adapter=codex_cli|codex_app_server] [codex_cli_command=<command>] [app_server_url=<url>]\n"
        << "  agentos autodev execute-next-task job_id=<job_id> [execution_adapter=codex_cli|codex_app_server] [codex_cli_command=<command>] [app_server_url=<url>]\n";
}

bool ParseBool(const std::string& value) {
    return value == "true" || value == "1" || value == "yes";
}

void PrintStringList(const std::string& label, const std::vector<std::string>& values);

int RunSubmit(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    AutoDevSubmitRequest request;
    request.agentos_workspace = workspace;
    if (const auto it = options.find("target_repo_path"); it != options.end()) {
        request.target_repo_path = it->second;
    }
    if (const auto it = options.find("objective"); it != options.end()) {
        request.objective = it->second;
    }
    if (const auto it = options.find("skill_pack_path"); it != options.end()) {
        request.skill_pack_path = std::filesystem::path(it->second);
    }
    if (const auto it = options.find("isolation_mode"); it != options.end()) {
        request.isolation_mode = it->second;
    }
    if (const auto it = options.find("allow_dirty_target"); it != options.end()) {
        request.allow_dirty_target = ParseBool(it->second);
    }

    AutoDevStateStore store(workspace);
    const auto result = store.submit(request);
    if (!result.success) {
        std::cerr << "autodev submit failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev job submitted\n"
              << "job_id:             " << result.job.job_id << '\n'
              << "status:             " << result.job.status << '\n'
              << "phase:              " << result.job.phase << '\n'
              << "current_activity:   " << result.job.current_activity << '\n'
              << "target_repo_path:   " << result.job.target_repo_path.string() << '\n'
              << "job_worktree_path:  " << result.job.job_worktree_path.string() << " (planned)\n"
              << "isolation_mode:     " << result.job.isolation_mode << '\n'
              << "isolation_status:   " << result.job.isolation_status << '\n'
              << "allow_dirty_target: " << (result.job.allow_dirty_target ? "true" : "false") << '\n'
              << "next_action:        " << result.job.next_action << '\n'
              << "skill_pack_status:  " << result.job.skill_pack.status << '\n'
              << "job_dir:            " << result.job_dir.string() << '\n'
              << "job_json:           " << store.job_json_path(result.job.job_id).string() << '\n'
              << "events:             " << store.events_path(result.job.job_id).string() << '\n'
              << "\nWorkspace is not ready yet. No target files have been modified.\n";
    return 0;
}

int RunPrepareWorkspace(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev prepare-workspace failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev prepare-workspace failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.prepare_workspace(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev prepare-workspace failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:           " << result.job.status << '\n'
                      << "phase:            " << result.job.phase << '\n'
                      << "isolation_status: " << result.job.isolation_status << '\n'
                      << "next_action:      " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev workspace prepared\n"
              << "job_id:                " << result.job.job_id << '\n'
              << "status:                " << result.job.status << '\n'
              << "phase:                 " << result.job.phase << '\n'
              << "isolation_status:      " << result.job.isolation_status << '\n'
              << "target_repo_path:      " << result.job.target_repo_path.string() << '\n'
              << "job_worktree_path:     " << result.job.job_worktree_path.string() << '\n'
              << "created_from_head_sha: " << result.job.created_from_head_sha.value_or("") << '\n'
              << "worktree_created_at:   " << result.job.worktree_created_at.value_or("") << '\n'
              << "next_action:           " << result.job.next_action << '\n'
              << "\nTarget working tree files were not modified; work will continue in the job worktree.\n";
    return 0;
}

int RunLoadSkillPack(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev load-skill-pack failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev load-skill-pack failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    std::optional<std::filesystem::path> skill_pack_path;
    if (const auto it = options.find("skill_pack_path"); it != options.end() && !it->second.empty()) {
        skill_pack_path = std::filesystem::path(it->second);
    }

    AutoDevStateStore store(workspace);
    const auto result = store.load_skill_pack(job_id_it->second, skill_pack_path);
    if (!result.success) {
        std::cerr << "autodev load-skill-pack failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:            " << result.job.status << '\n'
                      << "skill_pack_status: " << result.job.skill_pack.status << '\n'
                      << "next_action:       " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev skill pack loaded\n"
              << "job_id:             " << result.job.job_id << '\n'
              << "status:             " << result.job.status << '\n'
              << "phase:              " << result.job.phase << '\n'
              << "skill_pack_status:  " << result.job.skill_pack.status << '\n'
              << "skill_pack_path:    "
              << (result.job.skill_pack.local_path.has_value() ? result.job.skill_pack.local_path->string() : "")
              << '\n'
              << "manifest_hash:      " << result.job.skill_pack.manifest_hash.value_or("") << '\n'
              << "snapshot:           " << result.snapshot_path.string() << '\n'
              << "next_action:        " << result.job.next_action << '\n'
              << "\nNo docs/goal files were generated and no Codex execution was started.\n";
    return 0;
}

int RunGenerateGoalDocs(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev generate-goal-docs failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev generate-goal-docs failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.generate_goal_docs(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev generate-goal-docs failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev goal docs generated\n"
              << "job_id:        " << result.job.job_id << '\n'
              << "status:        " << result.job.status << '\n'
              << "phase:         " << result.job.phase << '\n'
              << "goal_dir:      " << result.goal_dir.string() << '\n'
              << "files_written: " << result.written_files.size() << '\n'
              << "next_action:   " << result.job.next_action << '\n'
              << "\nThese are candidate working documents only. No spec was frozen and no Codex execution was started.\n";
    return 0;
}

int RunValidateSpec(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev validate-spec failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev validate-spec failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.validate_spec(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev validate-spec failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev spec validated\n"
              << "job_id:        " << result.job.job_id << '\n'
              << "status:        " << result.job.status << '\n'
              << "phase:         " << result.job.phase << '\n'
              << "approval_gate: " << result.job.approval_gate << '\n'
              << "spec_revision: " << result.spec_revision << '\n'
              << "spec_hash:     " << result.spec_hash << '\n'
              << "normalized:    " << result.normalized_path.string() << '\n'
              << "hash_file:     " << result.hash_path.string() << '\n'
              << "status_file:   " << result.status_path.string() << '\n'
              << "next_action:   " << result.job.next_action << '\n'
              << "\nThe spec snapshot is pending approval. No spec was frozen and no Codex execution was started.\n";
    return 0;
}

int RunApproveSpec(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev approve-spec failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev approve-spec failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto hash_it = options.find("spec_hash");
    if (hash_it == options.end() || hash_it->second.empty()) {
        std::cerr << "autodev approve-spec failed: spec_hash is required\n";
        return 1;
    }
    std::optional<std::string> spec_revision;
    if (const auto it = options.find("spec_revision"); it != options.end() && !it->second.empty()) {
        spec_revision = it->second;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.approve_spec(job_id_it->second, hash_it->second, spec_revision);
    if (!result.success) {
        std::cerr << "autodev approve-spec failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev spec approved\n"
              << "job_id:        " << result.job.job_id << '\n'
              << "status:        " << result.job.status << '\n'
              << "phase:         " << result.job.phase << '\n'
              << "approval_gate: " << result.job.approval_gate << '\n'
              << "spec_revision: " << result.spec_revision << '\n'
              << "spec_hash:     " << result.spec_hash << '\n'
              << "status_file:   " << result.status_path.string() << '\n'
              << "tasks_file:    " << result.tasks_path.string() << '\n'
              << "next_action:   " << result.job.next_action << '\n'
              << "\nThe frozen spec is approved. Codex execution was not started by this command.\n";
    return 0;
}

int RunRecoverBlocked(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev recover-blocked failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev recover-blocked failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto loaded_job = store.load_job(job_id_it->second, &error);
    if (!loaded_job.has_value()) {
        std::cerr << "autodev recover-blocked failed: " << error << '\n';
        return 1;
    }

    const auto print_job = [](const AutoDevJob& job) {
        std::cout << "job_id:      " << job.job_id << '\n'
                  << "status:      " << job.status << '\n'
                  << "phase:       " << job.phase << '\n'
                  << "next_action: " << job.next_action << '\n';
        if (job.blocker.has_value()) {
            std::cout << "blocker:     " << *job.blocker << '\n';
        }
        const auto recovery_hint = AutoDevRecoveryHint(job);
        if (!recovery_hint.empty()) {
            std::cout << "recovery:    " << recovery_hint << '\n';
        }
    };

    const auto fail_with_job = [&](const AutoDevJob& job, const std::string& message) {
        std::cerr << "autodev recover-blocked failed: " << message << '\n';
        print_job(job);
        return 1;
    };

    const auto job = *loaded_job;
    if (job.status == "awaiting_approval" && job.next_action == "approve_spec") {
        return fail_with_job(job, "spec approval requires explicit approve-spec");
    }
    if (job.status != "blocked") {
        return fail_with_job(job, "job is not blocked");
    }

    std::string attempted_action;
    bool success = false;
    std::string failure;
    AutoDevJob result_job;

    if (job.next_action == "prepare_workspace" ||
        job.next_action == "commit_or_allow_dirty_target" ||
        job.next_action == "fix_workspace_or_use_git_worktree" ||
        job.next_action == "fix_workspace_or_use_in_place" ||
        job.next_action == "fix_workspace_path") {
        attempted_action = "prepare-workspace";
        const auto result = store.prepare_workspace(job.job_id);
        success = result.success;
        failure = result.error_message;
        result_job = result.job;
    } else if (job.next_action == "load_skill_pack" ||
               job.next_action == "declare_skill_pack" ||
               job.next_action == "fix_skill_pack") {
        attempted_action = "load-skill-pack";
        std::optional<std::filesystem::path> skill_pack_path;
        if (const auto it = options.find("skill_pack_path"); it != options.end() && !it->second.empty()) {
            skill_pack_path = std::filesystem::path(it->second);
        }
        const auto result = store.load_skill_pack(job.job_id, skill_pack_path);
        success = result.success;
        failure = result.error_message;
        result_job = result.job;
    } else if (job.next_action == "generate_goal_docs") {
        attempted_action = "generate-goal-docs";
        const auto result = store.generate_goal_docs(job.job_id);
        success = result.success;
        failure = result.error_message;
        result_job = result.job;
    } else if (job.next_action == "validate_spec" || job.next_action == "fix_autodev_spec") {
        attempted_action = "validate-spec";
        const auto result = store.validate_spec(job.job_id);
        success = result.success;
        failure = result.error_message;
        result_job = result.job;
    } else {
        return fail_with_job(job, "blocked next_action is not recoverable by this command");
    }

    if (!success) {
        std::cerr << "autodev recover-blocked failed while running " << attempted_action << ": " << failure << '\n';
        print_job(result_job.job_id.empty() ? job : result_job);
        return 1;
    }

    std::cout << "AutoDev blocked job recovered\n"
              << "attempted_action: " << attempted_action << '\n';
    print_job(result_job);
    std::cout << "Recovery command only reran the blocked runtime gate; it did not approve specs or start Codex execution.\n";
    return 0;
}

int RunRecoverCrash(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev recover-crash failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev recover-crash failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.recover_crash(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev recover-crash failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev crash recovery checked\n"
              << "job_id:              " << result.job.job_id << '\n'
              << "status:              " << result.job.status << '\n'
              << "phase:               " << result.job.phase << '\n'
              << "next_action:         " << result.job.next_action << '\n'
              << "blocked:             " << (result.blocked ? "true" : "false") << '\n'
              << "recovered_count:     " << result.recovered_count << '\n'
              << "stale_lock_removed:  " << (result.stale_lock_removed ? "true" : "false") << '\n';
    if (result.job.blocker.has_value()) {
        std::cout << "blocker:             " << *result.job.blocker << '\n';
    }
    PrintStringList("findings:", result.findings);
    return result.blocked ? 1 : 0;
}

int RunTasks(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev tasks failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev tasks failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto tasks = store.load_tasks(job_id_it->second, &error);
    if (!tasks.has_value()) {
        std::cerr << "autodev tasks failed: " << error << '\n';
        return 1;
    }
    if (WantsJson(options)) {
        PrintJson(nlohmann::json{
            {"job_id", job_id_it->second},
            {"total", tasks->size()},
            {"tasks", RecordsJson(*tasks)},
        });
        return 0;
    }

    std::size_t passed = 0;
    for (const auto& task : *tasks) {
        if (task.status == "passed") {
            ++passed;
        }
    }

    std::cout << "AutoDev tasks\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << tasks->size() << '\n'
              << "passed: " << passed << '\n';
    for (const auto& task : *tasks) {
        std::cout << '\n'
                  << "- task_id:          " << task.task_id << '\n'
                  << "  title:            " << task.title << '\n'
                  << "  status:           " << task.status << '\n'
                  << "  current_activity: " << task.current_activity << '\n'
                  << "  spec_revision:    " << task.spec_revision << '\n'
                  << "  retry:           " << task.retry_count << "/" << task.max_retries << '\n'
                  << "  acceptance:       " << task.acceptance_passed << "/" << task.acceptance_total << '\n';
        if (task.verify_command.has_value()) {
            std::cout << "  verify_command:   " << *task.verify_command << '\n';
        }
        if (!task.allowed_files.empty()) {
            std::cout << "  allowed_files:    ";
            for (std::size_t i = 0; i < task.allowed_files.size(); ++i) {
                if (i != 0) {
                    std::cout << ", ";
                }
                std::cout << task.allowed_files[i];
            }
            std::cout << '\n';
        }
        if (!task.blocked_files.empty()) {
            std::cout << "  blocked_files:    ";
            for (std::size_t i = 0; i < task.blocked_files.size(); ++i) {
                if (i != 0) {
                    std::cout << ", ";
                }
                std::cout << task.blocked_files[i];
            }
            std::cout << '\n';
        }
    }
    return 0;
}

int RunEvents(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev events failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev events failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto lines = store.load_event_lines(job_id_it->second, &error);
    if (!lines.has_value()) {
        std::cerr << "autodev events failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev events\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << lines->size() << '\n';
    for (const auto& line : *lines) {
        try {
            const auto event = nlohmann::json::parse(line);
            std::cout << "- " << event.value("at", "") << " "
                      << event.value("type", "unknown");
            if (event.contains("status") && event.at("status").is_string()) {
                std::cout << " status=" << event.at("status").get<std::string>();
            }
            if (event.contains("phase") && event.at("phase").is_string()) {
                std::cout << " phase=" << event.at("phase").get<std::string>();
            }
            if (event.contains("next_action") && event.at("next_action").is_string()) {
                std::cout << " next_action=" << event.at("next_action").get<std::string>();
            }
            if (event.contains("prompt_artifact") && event.at("prompt_artifact").is_string()) {
                std::cout << " prompt_artifact=" << event.at("prompt_artifact").get<std::string>();
            }
            if (event.contains("blocker") && event.at("blocker").is_string() && !event.at("blocker").get<std::string>().empty()) {
                std::cout << " blocker=" << event.at("blocker").get<std::string>();
            }
            std::cout << '\n';
        } catch (...) {
            std::cout << "- " << line << '\n';
        }
    }
    return 0;
}

int RunTurns(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev turns failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev turns failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto turns = store.load_turns(job_id_it->second, &error);
    if (!turns.has_value()) {
        std::cerr << "autodev turns failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev turns\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << turns->size() << '\n';
    for (const auto& turn : *turns) {
        std::cout << '\n'
                  << "- turn_id:           " << turn.turn_id << '\n'
                  << "  task_id:           " << turn.task_id << '\n'
                  << "  status:            " << turn.status << '\n'
                  << "  adapter_kind:      " << turn.adapter_kind << '\n'
                  << "  continuity_mode:   " << turn.continuity_mode << '\n'
                  << "  event_stream_mode: " << turn.event_stream_mode << '\n'
                  << "  session_id:        " << turn.session_id << '\n'
                  << "  started_at:        " << turn.started_at << '\n';
        if (turn.completed_at.has_value()) {
            std::cout << "  completed_at:      " << *turn.completed_at << '\n';
        }
        if (turn.error_code.has_value()) {
            std::cout << "  error_code:        " << *turn.error_code << '\n';
        }
        if (turn.error_message.has_value()) {
            std::cout << "  error_message:     " << *turn.error_message << '\n';
        }
        if (turn.prompt_artifact.has_value()) {
            std::cout << "  prompt_artifact:   " << turn.prompt_artifact->string() << '\n';
        }
        if (turn.response_artifact.has_value()) {
            std::cout << "  response_artifact: " << turn.response_artifact->string() << '\n';
        }
        if (!turn.changed_files.empty()) {
            std::cout << "  changed_files:     ";
            for (std::size_t i = 0; i < turn.changed_files.size(); ++i) {
                if (i != 0) {
                    std::cout << ", ";
                }
                std::cout << turn.changed_files[i];
            }
            std::cout << '\n';
        }
    }
    return 0;
}

int RunSnapshotTask(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    const auto task_id_it = options.find("task_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev snapshot-task failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev snapshot-task failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev snapshot-task failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.record_task_snapshot(job_id_it->second, task_id_it->second);
    if (!result.success) {
        std::cerr << "autodev snapshot-task failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev task snapshot recorded\n"
              << "job_id:       " << result.snapshot.job_id << '\n'
              << "task_id:      " << result.snapshot.task_id << '\n'
              << "snapshot_id:  " << result.snapshot.snapshot_id << '\n'
              << "head_sha:     " << result.snapshot.head_sha << '\n'
              << "status_lines: " << result.snapshot.git_status.size() << '\n'
              << "snapshots:    " << result.snapshots_path.string() << '\n'
              << "artifact:     " << result.snapshot_artifact_path.string() << '\n'
              << "\nSnapshot records runtime facts only. It does not roll back files.\n";
    return 0;
}

int RunSnapshots(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev snapshots failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev snapshots failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto snapshots = store.load_snapshots(job_id_it->second, &error);
    if (!snapshots.has_value()) {
        std::cerr << "autodev snapshots failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev snapshots\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << snapshots->size() << '\n';
    for (const auto& snapshot : *snapshots) {
        std::cout << '\n'
                  << "- snapshot_id: " << snapshot.snapshot_id << '\n'
                  << "  task_id:     " << snapshot.task_id << '\n'
                  << "  head_sha:    " << snapshot.head_sha << '\n'
                  << "  status_lines:" << ' ' << snapshot.git_status.size() << '\n'
                  << "  captured_at: " << snapshot.captured_at << '\n';
        if (snapshot.artifact_path.has_value()) {
            std::cout << "  artifact:    " << snapshot.artifact_path->string() << '\n';
        }
    }
    return 0;
}

int RunRollbackSoft(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    const auto task_id_it = options.find("task_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev rollback-soft failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev rollback-soft failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev rollback-soft failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.rollback_soft(job_id_it->second, task_id_it->second);
    if (!result.success) {
        std::cerr << "autodev rollback-soft failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev soft rollback recorded\n"
              << "job_id:      " << result.rollback.job_id << '\n'
              << "task_id:     " << result.rollback.task_id << '\n'
              << "rollback_id: " << result.rollback.rollback_id << '\n'
              << "status:      " << result.rollback.status << '\n'
              << "executed:    " << (result.rollback.executed ? "true" : "false") << '\n'
              << "destructive: false\n"
              << "rollbacks:   " << result.rollbacks_path.string() << '\n';
    PrintStringList("target_files:", result.rollback.target_files);
    std::cout << "reason:      " << result.rollback.reason << '\n'
              << "\nSoft rollback only restores tracked task files in the job worktree. It does not clean untracked files or touch the target repo.\n";
    return 0;
}

int RunRollbackHard(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    const auto task_id_it = options.find("task_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev rollback-hard failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev rollback-hard failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev rollback-hard failed: task_id is required\n";
        return 1;
    }
    std::optional<std::string> approval;
    if (const auto it = options.find("approval"); it != options.end() && !it->second.empty()) {
        approval = it->second;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.rollback_hard(job_id_it->second, task_id_it->second, approval);
    if (!result.success) {
        std::cerr << "autodev rollback-hard failed: " << result.error_message << '\n'
                  << "rollback_id: " << result.rollback.rollback_id << '\n'
                  << "destructive: true\n"
                  << "executed:    false\n";
        return 1;
    }
    return 0;
}

int RunRollbacks(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev rollbacks failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev rollbacks failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto rollbacks = store.load_rollbacks(job_id_it->second, &error);
    if (!rollbacks.has_value()) {
        std::cerr << "autodev rollbacks failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev rollbacks\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << rollbacks->size() << '\n';
    for (const auto& rollback : *rollbacks) {
        std::cout << '\n'
                  << "- rollback_id: " << rollback.rollback_id << '\n'
                  << "  task_id:     " << rollback.task_id << '\n'
                  << "  mode:        " << rollback.mode << '\n'
                  << "  status:      " << rollback.status << '\n'
                  << "  destructive: " << (rollback.destructive ? "true" : "false") << '\n'
                  << "  executed:    " << (rollback.executed ? "true" : "false") << '\n'
                  << "  recorded_at: " << rollback.recorded_at << '\n'
                  << "  reason:      " << rollback.reason << '\n';
        PrintStringList("  target_files:", rollback.target_files);
    }
    std::cout << "\nRollback records are runtime facts. This command does not modify the worktree.\n";
    return 0;
}

int RunRepairs(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev repairs failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev repairs failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto repairs = store.load_repairs(job_id_it->second, &error);
    if (!repairs.has_value()) {
        std::cerr << "autodev repairs failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev repairs\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << repairs->size() << '\n';
    for (const auto& repair : *repairs) {
        std::cout << '\n'
                  << "- repair_id:   " << repair.repair_id << '\n'
                  << "  task_id:     " << repair.task_id << '\n'
                  << "  source:      " << repair.source_type << " " << repair.source_id << '\n'
                  << "  status:      " << repair.status << '\n'
                  << "  next_action: " << repair.next_action << '\n'
                  << "  retry:       " << repair.retry_count << "/" << repair.max_retries << '\n'
                  << "  retry_limit_exceeded: " << (repair.retry_limit_exceeded ? "true" : "false") << '\n'
                  << "  recorded_at: " << repair.recorded_at << '\n';
        if (repair.prompt_artifact.has_value()) {
            std::cout << "  prompt_artifact: " << repair.prompt_artifact->string() << '\n';
        }
        PrintStringList("  reasons:", repair.reasons);
    }
    return 0;
}

int RunRepairNextOrTask(
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[],
    const bool require_task_id) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    const std::string command_name = require_task_id ? "repair-task" : "repair-next";
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev " << command_name << " failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev " << command_name << " failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    std::optional<std::string> requested_task_id;
    if (const auto it = options.find("task_id"); it != options.end() && !it->second.empty()) {
        requested_task_id = it->second;
    }
    if (require_task_id && !requested_task_id.has_value()) {
        std::cerr << "autodev repair-task failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto repairs = store.load_repairs(job_id_it->second, &error);
    if (!repairs.has_value()) {
        std::cerr << "autodev " << command_name << " failed: " << error << '\n';
        return 1;
    }
    if (repairs->empty()) {
        std::cerr << "autodev " << command_name << " failed: no repair-needed facts are recorded\n";
        return 1;
    }

    const AutoDevRepairNeeded* selected = nullptr;
    for (auto it = repairs->rbegin(); it != repairs->rend(); ++it) {
        if (requested_task_id.has_value() && it->task_id != *requested_task_id) {
            continue;
        }
        if (it->status == "needed" && !it->retry_limit_exceeded) {
            selected = &*it;
            break;
        }
    }
    if (!selected) {
        for (auto it = repairs->rbegin(); it != repairs->rend(); ++it) {
            if (!requested_task_id.has_value() || it->task_id == *requested_task_id) {
                selected = &*it;
                break;
            }
        }
    }
    if (!selected) {
        std::cerr << "autodev " << command_name << " failed: no repair fact found for task_id: "
                  << *requested_task_id << '\n';
        return 1;
    }
    if (!selected->prompt_artifact.has_value()) {
        std::cerr << "autodev " << command_name << " failed: selected repair has no prompt_artifact: "
                  << selected->repair_id << '\n';
        return 1;
    }

    std::string prompt_error;
    const auto prompt = ReadTextFileForCli(*selected->prompt_artifact, &prompt_error);
    if (!prompt_error.empty()) {
        std::cerr << "autodev " << command_name << " failed: " << prompt_error << '\n';
        return 1;
    }

    std::cout << "AutoDev repair task\n"
              << "job_id:       " << job_id_it->second << '\n'
              << "task_id:      " << selected->task_id << '\n'
              << "repair_id:    " << selected->repair_id << '\n'
              << "source:       " << selected->source_type << " " << selected->source_id << '\n'
              << "status:       " << selected->status << '\n'
              << "next_action:  " << selected->next_action << '\n'
              << "retry:        " << selected->retry_count << "/" << selected->max_retries << '\n'
              << "retry_limit_exceeded: " << (selected->retry_limit_exceeded ? "true" : "false") << '\n'
              << "prompt_artifact: " << selected->prompt_artifact->string() << '\n';
    PrintStringList("reasons:", selected->reasons);
    std::cout << "\nRepair prompt preview\n"
              << PreviewText(prompt)
              << "\nSame-task repair flow\n"
              << "1. Apply the prompt above in the same Codex thread/session for task_id=" << selected->task_id << ".\n"
              << "2. Run: agentos autodev execute-next-task job_id=" << job_id_it->second
              << " execution_adapter=codex_cli\n"
              << "3. Run: agentos autodev verify-task job_id=" << job_id_it->second
              << " task_id=" << selected->task_id << '\n'
              << "4. Run: agentos autodev diff-guard job_id=" << job_id_it->second
              << " task_id=" << selected->task_id << '\n'
              << "5. Run: agentos autodev acceptance-gate job_id=" << job_id_it->second
              << " task_id=" << selected->task_id << '\n';
    return selected->retry_limit_exceeded ? 1 : 0;
}

int RunVerifyTask(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev verify-task failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev verify-task failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto task_id_it = options.find("task_id");
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev verify-task failed: task_id is required\n";
        return 1;
    }
    std::optional<std::string> related_turn_id;
    if (const auto it = options.find("related_turn_id"); it != options.end() && !it->second.empty()) {
        related_turn_id = it->second;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.verify_task(job_id_it->second, task_id_it->second, related_turn_id);
    if (!result.success) {
        std::cerr << "autodev verify-task failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev task verified\n"
              << "job_id:          " << result.verification.job_id << '\n'
              << "task_id:         " << result.verification.task_id << '\n'
              << "verification_id: " << result.verification.verification_id << '\n'
              << "passed:          " << (result.verification.passed ? "true" : "false") << '\n'
              << "exit_code:       " << result.verification.exit_code << '\n'
              << "duration_ms:     " << result.verification.duration_ms << '\n'
              << "command:         " << result.verification.command << '\n'
              << "cwd:             " << result.verification.cwd.string() << '\n'
              << "verification:    " << result.verification_path.string() << '\n';
    if (result.verification.output_log_path.has_value()) {
        std::cout << "output_log:      " << result.verification.output_log_path->string() << '\n';
    }
    std::cout << "verify_report:   " << result.verify_report_path.string() << '\n';
    std::cout << "\nVerification facts were recorded. AcceptanceGate was not run and task status was not changed.\n";
    return result.verification.passed ? 0 : 1;
}

int RunVerifications(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev verifications failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev verifications failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto verifications = store.load_verifications(job_id_it->second, &error);
    if (!verifications.has_value()) {
        std::cerr << "autodev verifications failed: " << error << '\n';
        return 1;
    }
    if (WantsJson(options)) {
        PrintJson(nlohmann::json{
            {"job_id", job_id_it->second},
            {"total", verifications->size()},
            {"verifications", RecordsJson(*verifications)},
        });
        return 0;
    }

    std::cout << "AutoDev verifications\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << verifications->size() << '\n';
    for (const auto& verification : *verifications) {
        std::cout << '\n'
                  << "- verification_id: " << verification.verification_id << '\n'
                  << "  task_id:         " << verification.task_id << '\n'
                  << "  passed:          " << (verification.passed ? "true" : "false") << '\n'
                  << "  exit_code:       " << verification.exit_code << '\n'
                  << "  duration_ms:     " << verification.duration_ms << '\n'
                  << "  command:         " << verification.command << '\n'
                  << "  cwd:             " << verification.cwd.string() << '\n';
        if (verification.output_log_path.has_value()) {
            std::cout << "  output_log:      " << verification.output_log_path->string() << '\n';
        }
        if (verification.related_turn_id.has_value()) {
            std::cout << "  related_turn_id: " << *verification.related_turn_id << '\n';
        }
    }
    return 0;
}

void PrintStringList(const std::string& label, const std::vector<std::string>& values) {
    std::cout << label;
    if (values.empty()) {
        std::cout << " none\n";
        return;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            std::cout << ",";
        }
        std::cout << " " << values[i];
    }
    std::cout << '\n';
}

int RunDiffGuard(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev diff-guard failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev diff-guard failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto task_id_it = options.find("task_id");
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev diff-guard failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.diff_guard(job_id_it->second, task_id_it->second);
    if (!result.success) {
        std::cerr << "autodev diff-guard failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev diff guard checked\n"
              << "job_id:  " << result.diff_guard.job_id << '\n'
              << "task_id: " << result.diff_guard.task_id << '\n'
              << "diff_id: " << result.diff_guard.diff_id << '\n'
              << "passed:  " << (result.diff_guard.passed ? "true" : "false") << '\n'
              << "diffs:   " << result.diffs_path.string() << '\n';
    PrintStringList("changed_files:           ", result.diff_guard.changed_files);
    PrintStringList("blocked_file_violations:", result.diff_guard.blocked_file_violations);
    PrintStringList("outside_allowed_files:  ", result.diff_guard.outside_allowed_files);
    std::cout << "\nDiffGuard facts were recorded. AcceptanceGate was not run and task status was not changed.\n";
    return result.diff_guard.passed ? 0 : 1;
}

int RunDiffs(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev diffs failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev diffs failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto diffs = store.load_diffs(job_id_it->second, &error);
    if (!diffs.has_value()) {
        std::cerr << "autodev diffs failed: " << error << '\n';
        return 1;
    }
    if (WantsJson(options)) {
        PrintJson(nlohmann::json{
            {"job_id", job_id_it->second},
            {"total", diffs->size()},
            {"diffs", RecordsJson(*diffs)},
        });
        return 0;
    }

    std::cout << "AutoDev diffs\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << diffs->size() << '\n';
    for (const auto& diff : *diffs) {
        std::cout << '\n'
                  << "- diff_id: " << diff.diff_id << '\n'
                  << "  task_id: " << diff.task_id << '\n'
                  << "  passed:  " << (diff.passed ? "true" : "false") << '\n';
        PrintStringList("  changed_files:           ", diff.changed_files);
        PrintStringList("  blocked_file_violations:", diff.blocked_file_violations);
        PrintStringList("  outside_allowed_files:  ", diff.outside_allowed_files);
    }
    return 0;
}

int RunAcceptanceGate(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev acceptance-gate failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev acceptance-gate failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto task_id_it = options.find("task_id");
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev acceptance-gate failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.acceptance_gate(job_id_it->second, task_id_it->second);
    if (!result.success) {
        std::cerr << "autodev acceptance-gate failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev acceptance gate checked\n"
              << "job_id:        " << result.acceptance.job_id << '\n'
              << "task_id:       " << result.acceptance.task_id << '\n'
              << "acceptance_id: " << result.acceptance.acceptance_id << '\n'
              << "passed:        " << (result.acceptance.passed ? "true" : "false") << '\n'
              << "task_status:   " << result.task.status << '\n'
              << "verification:  " << result.acceptance.verification_id.value_or("(none)") << '\n'
              << "diff:          " << result.acceptance.diff_id.value_or("(none)") << '\n'
              << "acceptance:    " << result.acceptance_path.string() << '\n';
    PrintStringList("reasons:       ", result.acceptance.reasons);
    std::cout << "\nAcceptanceGate updated only the task when runtime facts passed. Job completion was not evaluated.\n";
    return result.acceptance.passed ? 0 : 1;
}

int RunAcceptances(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev acceptances failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev acceptances failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto acceptances = store.load_acceptances(job_id_it->second, &error);
    if (!acceptances.has_value()) {
        std::cerr << "autodev acceptances failed: " << error << '\n';
        return 1;
    }
    if (WantsJson(options)) {
        PrintJson(nlohmann::json{
            {"job_id", job_id_it->second},
            {"total", acceptances->size()},
            {"acceptances", RecordsJson(*acceptances)},
        });
        return 0;
    }

    std::cout << "AutoDev acceptances\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << acceptances->size() << '\n';
    for (const auto& acceptance : *acceptances) {
        std::cout << '\n'
                  << "- acceptance_id: " << acceptance.acceptance_id << '\n'
                  << "  task_id:       " << acceptance.task_id << '\n'
                  << "  passed:        " << (acceptance.passed ? "true" : "false") << '\n'
                  << "  verification:  " << acceptance.verification_id.value_or("(none)") << '\n'
                  << "  diff:          " << acceptance.diff_id.value_or("(none)") << '\n'
                  << "  checked_at:    " << acceptance.checked_at << '\n';
        PrintStringList("  reasons:       ", acceptance.reasons);
    }
    return 0;
}

int RunFinalReview(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev final-review failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev final-review failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.final_review(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev final-review failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev final review checked\n"
              << "job_id:          " << result.final_review.job_id << '\n'
              << "final_review_id: " << result.final_review.final_review_id << '\n'
              << "passed:          " << (result.final_review.passed ? "true" : "false") << '\n'
              << "job_status:      " << result.job.status << '\n'
              << "job_phase:       " << result.job.phase << '\n'
              << "tasks:           " << result.final_review.tasks_passed << "/" << result.final_review.tasks_total << '\n'
              << "final_review:    " << result.final_review_path.string() << '\n'
              << "review_report:   " << result.final_review_report_path.string() << '\n';
    PrintStringList("changed_files:           ", result.final_review.changed_files);
    PrintStringList("blocked_file_violations:", result.final_review.blocked_file_violations);
    PrintStringList("outside_allowed_files:  ", result.final_review.outside_allowed_files);
    PrintStringList("reasons:                ", result.final_review.reasons);
    std::cout << "\nFinalReview records runtime facts and may advance the job to pr_ready. It never marks the job done.\n";
    return result.final_review.passed ? 0 : 1;
}

int RunFinalReviews(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev final-reviews failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev final-reviews failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto final_reviews = store.load_final_reviews(job_id_it->second, &error);
    if (!final_reviews.has_value()) {
        std::cerr << "autodev final-reviews failed: " << error << '\n';
        return 1;
    }
    if (WantsJson(options)) {
        PrintJson(nlohmann::json{
            {"job_id", job_id_it->second},
            {"total", final_reviews->size()},
            {"final_reviews", RecordsJson(*final_reviews)},
        });
        return 0;
    }

    std::cout << "AutoDev final reviews\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << final_reviews->size() << '\n';
    for (const auto& final_review : *final_reviews) {
        std::cout << '\n'
                  << "- final_review_id: " << final_review.final_review_id << '\n'
                  << "  passed:          " << (final_review.passed ? "true" : "false") << '\n'
                  << "  tasks:           " << final_review.tasks_passed << "/" << final_review.tasks_total << '\n'
                  << "  checked_at:      " << final_review.checked_at << '\n';
        PrintStringList("  changed_files:           ", final_review.changed_files);
        PrintStringList("  blocked_file_violations:", final_review.blocked_file_violations);
        PrintStringList("  outside_allowed_files:  ", final_review.outside_allowed_files);
        PrintStringList("  reasons:                ", final_review.reasons);
    }
    return 0;
}

int RunCompleteJob(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev complete-job failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev complete-job failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.complete_job(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev complete-job failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev job completed\n"
              << "job_id:          " << result.job.job_id << '\n'
              << "status:          " << result.job.status << '\n'
              << "phase:           " << result.job.phase << '\n'
              << "final_review_id: " << result.final_review.final_review_id << '\n'
              << "final_review:    passed\n"
              << "\nJob completion is controlled by AgentOS runtime facts, not Codex or Markdown summaries.\n";
    return 0;
}

int RunJobControl(
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[],
    const std::string& command,
    AutoDevJobControlResult (AutoDevStateStore::*operation)(const std::string&)) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev " << command << " failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev " << command << " failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = (store.*operation)(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev " << command << " failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    const auto past_tense = command == "cancel" ? std::string("cancelled") : command + "d";
    std::cout << "AutoDev job " << past_tense << '\n'
              << "job_id:      " << result.job.job_id << '\n'
              << "status:      " << result.job.status << '\n'
              << "phase:       " << result.job.phase << '\n'
              << "next_action: " << result.job.next_action << '\n'
              << "updated_at:  " << result.job.updated_at << '\n'
              << "\nNo Codex process was interrupted or terminated by this command.\n";
    return 0;
}

int RunCleanupWorktree(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev cleanup-worktree failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev cleanup-worktree failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.cleanup_worktree(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev cleanup-worktree failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "policy:      " << result.job.worktree_cleanup_policy << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev worktree cleaned\n"
              << "job_id:             " << result.job.job_id << '\n'
              << "status:             " << result.job.status << '\n'
              << "phase:              " << result.job.phase << '\n'
              << "isolation_status:   " << result.job.isolation_status << '\n'
              << "job_worktree_path:  " << result.cleaned_path.string() << '\n'
              << "removed:            " << (result.removed ? "true" : "false") << '\n'
              << "\nRuntime facts were preserved under AgentOS runtime store.\n";
    return 0;
}

int RunPrSummary(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev pr-summary failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev pr-summary failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << "autodev pr-summary failed: " << error << '\n';
        return 1;
    }
    if (job->status != "pr_ready" && job->status != "done") {
        std::cerr << "autodev pr-summary failed: job is not pr_ready\n";
        return 1;
    }
    const auto tasks = store.load_tasks(job_id_it->second, &error);
    if (!tasks.has_value()) {
        std::cerr << "autodev pr-summary failed: " << error << '\n';
        return 1;
    }
    const auto verifications = store.load_verifications(job_id_it->second, nullptr).value_or(std::vector<AutoDevVerification>{});
    const auto diffs = store.load_diffs(job_id_it->second, nullptr).value_or(std::vector<AutoDevDiffGuard>{});
    const auto acceptances = store.load_acceptances(job_id_it->second, nullptr).value_or(std::vector<AutoDevAcceptanceGate>{});
    const auto final_reviews = store.load_final_reviews(job_id_it->second, nullptr).value_or(std::vector<AutoDevFinalReview>{});
    if (final_reviews.empty() || !final_reviews.back().passed) {
        std::cerr << "autodev pr-summary failed: latest final review did not pass\n";
        return 1;
    }
    const auto& final_review = final_reviews.back();

    const auto latest_verification = [&verifications](const std::string& task_id) -> std::optional<AutoDevVerification> {
        for (auto it = verifications.rbegin(); it != verifications.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };
    const auto latest_diff = [&diffs](const std::string& task_id) -> std::optional<AutoDevDiffGuard> {
        for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };
    const auto latest_acceptance = [&acceptances](const std::string& task_id) -> std::optional<AutoDevAcceptanceGate> {
        for (auto it = acceptances.rbegin(); it != acceptances.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };

    std::cout << "AutoDev PR summary\n"
              << "job_id: " << job->job_id << '\n'
              << "status: " << job->status << '\n'
              << "phase:  " << job->phase << '\n'
              << '\n'
              << "Summary:\n"
              << "- objective: " << job->objective << '\n'
              << "- final_review: " << final_review.final_review_id << " passed=true tasks="
              << final_review.tasks_passed << "/" << final_review.tasks_total << '\n';
    PrintStringList("- changed_files:", final_review.changed_files);

    std::cout << "\nTasks:\n";
    for (const auto& task : *tasks) {
        const auto verification = latest_verification(task.task_id);
        const auto diff = latest_diff(task.task_id);
        const auto acceptance = latest_acceptance(task.task_id);
        std::cout << "- " << task.task_id << ": " << task.title << '\n'
                  << "  status: " << task.status << '\n'
                  << "  verification: "
                  << (verification.has_value() ? verification->verification_id : "(none)")
                  << " passed=" << (verification.has_value() && verification->passed ? "true" : "false") << '\n'
                  << "  diff_guard: "
                  << (diff.has_value() ? diff->diff_id : "(none)")
                  << " passed=" << (diff.has_value() && diff->passed ? "true" : "false") << '\n'
                  << "  acceptance: "
                  << (acceptance.has_value() ? acceptance->acceptance_id : "(none)")
                  << " passed=" << (acceptance.has_value() && acceptance->passed ? "true" : "false") << '\n';
    }

    std::cout << "\nCommands run:\n";
    if (verifications.empty()) {
        std::cout << "- none\n";
    } else {
        for (const auto& verification : verifications) {
            std::cout << "- " << verification.verification_id
                      << " task=" << verification.task_id
                      << " exit_code=" << verification.exit_code
                      << " command=" << verification.command << '\n';
        }
    }
    return 0;
}

int RunSummary(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev summary failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev summary failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << "autodev summary failed: " << error << '\n';
        return 1;
    }
    const auto loaded_tasks = store.load_tasks(job_id_it->second, nullptr);
    const auto tasks = loaded_tasks.value_or(std::vector<AutoDevTask>{});

    const auto snapshots = store.load_snapshots(job_id_it->second, nullptr).value_or(std::vector<AutoDevSnapshot>{});
    const auto verifications = store.load_verifications(job_id_it->second, nullptr).value_or(std::vector<AutoDevVerification>{});
    const auto diffs = store.load_diffs(job_id_it->second, nullptr).value_or(std::vector<AutoDevDiffGuard>{});
    const auto acceptances = store.load_acceptances(job_id_it->second, nullptr).value_or(std::vector<AutoDevAcceptanceGate>{});
    const auto final_reviews = store.load_final_reviews(job_id_it->second, nullptr).value_or(std::vector<AutoDevFinalReview>{});
    const auto repairs = store.load_repairs(job_id_it->second, nullptr).value_or(std::vector<AutoDevRepairNeeded>{});

    const auto latest_verification = [&verifications](const std::string& task_id) -> std::optional<AutoDevVerification> {
        for (auto it = verifications.rbegin(); it != verifications.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };
    const auto latest_diff = [&diffs](const std::string& task_id) -> std::optional<AutoDevDiffGuard> {
        for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };
    const auto latest_acceptance = [&acceptances](const std::string& task_id) -> std::optional<AutoDevAcceptanceGate> {
        for (auto it = acceptances.rbegin(); it != acceptances.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };

    std::size_t passed_tasks = 0;
    for (const auto& task : tasks) {
        if (task.status == "passed") {
            ++passed_tasks;
        }
    }
    const auto progress = ComputeProgress(*job, tasks);
    if (WantsJson(options)) {
        PrintJson(nlohmann::json{
            {"job", ToJson(*job)},
            {"progress", ProgressJson(progress)},
            {"fact_counts", {
                {"snapshots", snapshots.size()},
                {"verifications", verifications.size()},
                {"diffs", diffs.size()},
                {"acceptances", acceptances.size()},
                {"final_reviews", final_reviews.size()},
                {"repairs", repairs.size()},
            }},
            {"tasks", RecordsJson(tasks)},
            {"snapshots", RecordsJson(snapshots)},
            {"verifications", RecordsJson(verifications)},
            {"diffs", RecordsJson(diffs)},
            {"acceptances", RecordsJson(acceptances)},
            {"final_reviews", RecordsJson(final_reviews)},
            {"repairs", RecordsJson(repairs)},
        });
        return 0;
    }

    std::cout << "AutoDev summary\n"
              << "job_id:        " << job->job_id << '\n'
              << "status:        " << job->status << '\n'
              << "phase:         " << job->phase << '\n'
              << "next_action:   " << job->next_action << '\n'
              << "spec_revision: " << job->spec_revision.value_or("(none)") << '\n'
              << "tasks:         " << passed_tasks << "/" << tasks.size() << '\n'
              << "facts:         snapshots=" << snapshots.size()
              << " verifications=" << verifications.size()
              << " diffs=" << diffs.size()
              << " acceptances=" << acceptances.size()
              << " final_reviews=" << final_reviews.size()
              << " repairs=" << repairs.size() << '\n';
    if (job->blocker.has_value()) {
        std::cout << "blocker:       " << *job->blocker << '\n';
    }
    const auto recovery_hint = AutoDevRecoveryHint(*job);
    if (!recovery_hint.empty()) {
        std::cout << "recovery:      " << recovery_hint << '\n';
    }

    std::cout << "\nProgress:\n";
    PrintProgress("  ", progress);

    std::cout << "\nTasks:\n";
    for (const auto& task : tasks) {
        const auto verification = latest_verification(task.task_id);
        const auto diff = latest_diff(task.task_id);
        const auto acceptance = latest_acceptance(task.task_id);
        std::cout << "- " << task.task_id << " " << task.status << " - " << task.title << '\n'
                  << "  retry:        " << task.retry_count << "/" << task.max_retries << '\n'
                  << "  verification: "
                  << (verification.has_value() ? verification->verification_id : "(none)");
        if (verification.has_value()) {
            std::cout << " passed=" << (verification->passed ? "true" : "false");
        }
        std::cout << '\n'
                  << "  diff_guard:   "
                  << (diff.has_value() ? diff->diff_id : "(none)");
        if (diff.has_value()) {
            std::cout << " passed=" << (diff->passed ? "true" : "false");
        }
        std::cout << '\n'
                  << "  acceptance:   "
                  << (acceptance.has_value() ? acceptance->acceptance_id : "(none)");
        if (acceptance.has_value()) {
            std::cout << " passed=" << (acceptance->passed ? "true" : "false");
        }
        std::cout << '\n';
        if (diff.has_value() && (!diff->blocked_file_violations.empty() || !diff->outside_allowed_files.empty())) {
            PrintStringList("  diff_blocked_file_violations:", diff->blocked_file_violations);
            PrintStringList("  diff_outside_allowed_files:  ", diff->outside_allowed_files);
        }
        if (acceptance.has_value() && !acceptance->reasons.empty()) {
            PrintStringList("  acceptance_reasons:", acceptance->reasons);
        }
        if (verification.has_value() && !verification->passed) {
            std::cout << "  verification_exit_code: " << verification->exit_code << '\n';
        }
    }

    std::cout << "\nFinal review:\n";
    if (final_reviews.empty()) {
        std::cout << "  final_review_id: (none)\n";
    } else {
        const auto& final_review = final_reviews.back();
        std::cout << "  final_review_id: " << final_review.final_review_id << '\n'
                  << "  passed:          " << (final_review.passed ? "true" : "false") << '\n'
                  << "  tasks:           " << final_review.tasks_passed << "/" << final_review.tasks_total << '\n';
        PrintStringList("  reasons:         ", final_review.reasons);
    }
    std::cout << "\nRepair:\n";
    if (repairs.empty()) {
        std::cout << "  repair_id: (none)\n";
    } else {
        const auto& repair = repairs.back();
        std::cout << "  repair_id:   " << repair.repair_id << '\n'
                  << "  task_id:     " << repair.task_id << '\n'
                  << "  source:      " << repair.source_type << " " << repair.source_id << '\n'
                  << "  next_action: " << repair.next_action << '\n'
                  << "  retry:       " << repair.retry_count << "/" << repair.max_retries << '\n'
                  << "  retry_limit_exceeded: " << (repair.retry_limit_exceeded ? "true" : "false") << '\n';
        if (repair.prompt_artifact.has_value()) {
            std::cout << "  prompt_artifact: " << repair.prompt_artifact->string() << '\n';
        }
        PrintStringList("  reasons:", repair.reasons);
    }
    return 0;
}

int RunExecuteNextTask(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev execute-next-task failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev execute-next-task failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << "autodev execute-next-task failed: " << error << '\n';
        return 1;
    }
    if (job->status != "running" || job->phase != "codex_execution") {
        std::cerr << "autodev execute-next-task failed: job is not in codex_execution\n"
                  << "status: " << job->status << '\n'
                  << "phase:  " << job->phase << '\n';
        return 1;
    }
    if (job->approval_gate != "none") {
        std::cerr << "autodev execute-next-task failed: approval gate is still active: " << job->approval_gate << '\n';
        return 1;
    }
    if (job->isolation_status != "ready" || !std::filesystem::exists(job->job_worktree_path)) {
        std::cerr << "autodev execute-next-task failed: workspace isolation is not ready\n"
                  << "isolation_status: " << job->isolation_status << '\n'
                  << "job_worktree_path: " << job->job_worktree_path.string() << '\n';
        return 1;
    }
    if (!job->spec_revision.has_value() || !job->spec_hash.has_value()) {
        std::cerr << "autodev execute-next-task failed: no approved frozen spec is recorded on the job\n";
        return 1;
    }

    const auto tasks = store.load_tasks(job_id_it->second, &error);
    if (!tasks.has_value()) {
        std::cerr << "autodev execute-next-task failed: " << error << '\n';
        return 1;
    }
    const auto pending_task = std::find_if(tasks->begin(), tasks->end(), [](const AutoDevTask& task) {
        return task.status == "pending";
    });
    if (pending_task == tasks->end()) {
        std::cerr << "autodev execute-next-task failed: no pending AutoDev task is available\n";
        return 1;
    }

    const auto adapter_it = options.find("execution_adapter");
    const auto adapter_kind = adapter_it == options.end() || adapter_it->second.empty()
        ? std::string("codex_cli")
        : adapter_it->second;
    AutoDevExecutionAdapterProfile profile;
    bool adapter_healthy = false;
    std::string blocked_reason;
    std::string codex_cli_command;
    std::string app_server_url;
    if (adapter_kind == "codex_cli") {
        const CodexCliAutoDevAdapter adapter;
        profile = adapter.profile();
        adapter_healthy = adapter.healthy();
        if (const auto command_it = options.find("codex_cli_command");
            command_it != options.end() && !command_it->second.empty()) {
            codex_cli_command = command_it->second;
        } else if (const char* env_command = std::getenv("AGENTOS_AUTODEV_CODEX_CLI_COMMAND");
                   env_command != nullptr && std::string(env_command).size() > 0) {
            codex_cli_command = env_command;
        } else {
            codex_cli_command = "codex exec --skip-git-repo-check --sandbox workspace-write -";
        }
    } else if (adapter_kind == "codex_app_server") {
        if (const auto url_it = options.find("app_server_url"); url_it != options.end() && !url_it->second.empty()) {
            app_server_url = url_it->second;
        } else if (const char* env_url = std::getenv("AGENTOS_AUTODEV_CODEX_APP_SERVER_URL");
                   env_url != nullptr && std::string(env_url).size() > 0) {
            app_server_url = env_url;
        }
        const CodexAppServerAutoDevAdapter adapter(app_server_url);
        profile = adapter.profile();
        adapter_healthy = adapter.healthy();
        blocked_reason = "Codex app-server AutoDev execution is not implemented in this build";
    } else {
        std::cerr << "autodev execute-next-task failed: unsupported execution_adapter: " << adapter_kind << '\n'
                  << "supported: codex_cli, codex_app_server\n";
        return 1;
    }

    const auto snapshot = store.record_task_snapshot(job_id_it->second, pending_task->task_id);
    if (!snapshot.success) {
        std::cerr << "autodev execute-next-task failed: " << snapshot.error_message << '\n';
        return 1;
    }
    if (adapter_kind == "codex_cli") {
        const auto input_dir = store.job_dir(job_id_it->second) / "prompts";
        std::filesystem::create_directories(input_dir);
        const auto input_path = input_dir / ("codex-cli-input-" + pending_task->task_id + ".md");
        {
            std::ofstream prompt(input_path, std::ios::binary | std::ios::trunc);
            prompt << BuildCodexCliPrompt(*job, *pending_task);
        }
        const auto command = codex_cli_command + " < " + ShellQuote(input_path.string());
        const auto execution = RunShellCommandWithAutoDevControl(
            job->job_worktree_path,
            command,
            workspace,
            job_id_it->second);
        const auto turn = store.record_execution_turn(
            job_id_it->second,
            pending_task->task_id,
            profile,
            codex_cli_command,
            execution.exit_code,
            execution.duration_ms,
            execution.output);
        if (!turn.success) {
            std::cerr << "autodev execute-next-task failed: " << turn.error_message << '\n';
            return 1;
        }
        std::cout << "AutoDev execution completed\n"
                  << "job_id:             " << turn.job.job_id << '\n'
                  << "status:             " << turn.job.status << '\n'
                  << "phase:              " << turn.job.phase << '\n'
                  << "next_action:        " << turn.job.next_action << '\n'
                  << "task_id:            " << turn.task.task_id << '\n'
                  << "turn_id:            " << turn.turn.turn_id << '\n'
                  << "turn_status:        " << turn.turn.status << '\n'
                  << "exit_code:          " << execution.exit_code << '\n'
                  << "duration_ms:        " << execution.duration_ms << '\n'
                  << "prompt_artifact:    " << turn.turn.prompt_artifact->string() << '\n'
                  << "response_artifact:  " << turn.turn.response_artifact->string() << '\n'
                  << "changed_files:      " << turn.turn.changed_files.size() << '\n'
                  << "snapshot_id:        " << snapshot.snapshot.snapshot_id << '\n'
                  << "adapter_kind:       " << profile.adapter_kind << '\n'
                  << "adapter_name:       " << profile.adapter_name << '\n'
                  << "healthy:            " << (adapter_healthy ? "true" : "false") << '\n'
                  << "\nTask status and job completion remain controlled by AgentOS runtime facts.\n";
        return execution.exit_code == 0 ? 0 : 1;
    }
    if (adapter_kind == "codex_app_server" && !app_server_url.empty()) {
        const CodexAppServerAutoDevAdapter adapter(app_server_url);
        if (!adapter_healthy) {
            std::cerr << "autodev execute-next-task failed: Codex app-server is not healthy\n"
                      << "app_server_url: " << app_server_url << '\n';
            return 1;
        }
        int exit_code = -1;
        std::vector<std::string> events;
        std::string output;
        std::string session_id;
        try {
            session_id = adapter.start_session(job->job_id, pending_task->task_id);
            output = adapter.run_turn(session_id, BuildCodexCliPrompt(*job, *pending_task), &exit_code, &events);
        } catch (const std::exception& e) {
            std::cerr << "autodev execute-next-task failed: " << e.what() << '\n';
            return 1;
        }
        if (!events.empty()) {
            output += output.empty() || output.back() == '\n' ? "" : "\n";
            output += "[app-server events]\n";
            for (const auto& event : events) {
                output += event;
                output += "\n";
            }
        }
        const auto turn = store.record_execution_turn(
            job_id_it->second,
            pending_task->task_id,
            profile,
            "codex_app_server " + app_server_url + " session=" + session_id,
            exit_code,
            0,
            output);
        if (!turn.success) {
            std::cerr << "autodev execute-next-task failed: " << turn.error_message << '\n';
            return 1;
        }
        std::cout << "AutoDev execution completed\n"
                  << "job_id:             " << turn.job.job_id << '\n'
                  << "status:             " << turn.job.status << '\n'
                  << "phase:              " << turn.job.phase << '\n'
                  << "next_action:        " << turn.job.next_action << '\n'
                  << "task_id:            " << turn.task.task_id << '\n'
                  << "turn_id:            " << turn.turn.turn_id << '\n'
                  << "turn_status:        " << turn.turn.status << '\n'
                  << "exit_code:          " << exit_code << '\n'
                  << "session_id:         " << session_id << '\n'
                  << "event_count:        " << events.size() << '\n'
                  << "prompt_artifact:    " << turn.turn.prompt_artifact->string() << '\n'
                  << "response_artifact:  " << turn.turn.response_artifact->string() << '\n'
                  << "changed_files:      " << turn.turn.changed_files.size() << '\n'
                  << "snapshot_id:        " << snapshot.snapshot.snapshot_id << '\n'
                  << "adapter_kind:       " << profile.adapter_kind << '\n'
                  << "adapter_name:       " << profile.adapter_name << '\n'
                  << "healthy:            true\n"
                  << "\nTask status and job completion remain controlled by AgentOS runtime facts.\n";
        return exit_code == 0 ? 0 : 1;
    }
    store.record_execution_blocked(*job, *pending_task, profile, blocked_reason);
    std::cout << "AutoDev execution preflight\n"
              << "job_id:             " << job->job_id << '\n'
              << "status:             " << job->status << '\n'
              << "phase:              " << job->phase << '\n'
              << "spec_revision:      " << *job->spec_revision << '\n'
              << "spec_hash:          " << *job->spec_hash << '\n'
              << "job_worktree_path:  " << job->job_worktree_path.string() << '\n'
              << '\n'
              << "Next task:\n"
              << "  task_id:          " << pending_task->task_id << '\n'
              << "  title:            " << pending_task->title << '\n'
              << "  status:           " << pending_task->status << '\n'
              << "  current_activity: " << pending_task->current_activity << '\n';
    if (pending_task->verify_command.has_value()) {
        std::cout << "  verify_command:   " << *pending_task->verify_command << '\n';
    }

    std::cout << '\n'
              << "Pre-task snapshot:\n"
              << "  snapshot_id:      " << snapshot.snapshot.snapshot_id << '\n'
              << "  head_sha:         " << snapshot.snapshot.head_sha << '\n'
              << "  status_lines:     " << snapshot.snapshot.git_status.size() << '\n'
              << "  artifact:         " << snapshot.snapshot_artifact_path.string() << '\n';

    std::cout << '\n'
              << "Execution adapter:\n"
              << "  adapter_kind:                " << profile.adapter_kind << '\n'
              << "  adapter_name:                " << profile.adapter_name << '\n'
              << "  continuity_mode:             " << profile.continuity_mode << '\n'
              << "  event_stream_mode:           " << profile.event_stream_mode << '\n'
              << "  supports_persistent_session: " << (profile.supports_persistent_session ? "true" : "false") << '\n'
              << "  supports_native_event_stream:" << (profile.supports_native_event_stream ? " true" : " false") << '\n'
              << "  supports_same_thread_repair: " << (profile.supports_same_thread_repair ? "true" : "false") << '\n'
              << "  production_final_executor:   " << (profile.production_final_executor ? "true" : "false") << '\n'
              << "  healthy:                     " << (adapter_healthy ? "true" : "false") << '\n'
              << "  risk_level:                  " << profile.risk_level << '\n'
              << '\n'
              << "Execution was not started. " << blocked_reason << ".\n"
              << "Task status and job completion remain controlled by AgentOS runtime facts.\n";
    return 1;
}

void PrintPipelineRepairHint(AutoDevStateStore& store, const std::string& job_id, const std::string& task_id) {
    const auto repairs = store.load_repairs(job_id, nullptr);
    if (!repairs.has_value()) {
        return;
    }
    for (auto it = repairs->rbegin(); it != repairs->rend(); ++it) {
        if (it->task_id != task_id) {
            continue;
        }
        std::cout << "repair_id:       " << it->repair_id << '\n'
                  << "repair_source:   " << it->source_type << " " << it->source_id << '\n'
                  << "repair_retry:    " << it->retry_count << "/" << it->max_retries << '\n'
                  << "repair_command:  agentos autodev repair-task job_id=" << job_id
                  << " task_id=" << task_id << '\n';
        if (it->prompt_artifact.has_value()) {
            std::cout << "repair_prompt:   " << it->prompt_artifact->string() << '\n';
        }
        return;
    }
}

int RunTaskPipeline(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev run-task failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev run-task failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    std::size_t existing_turn_count = 0;
    if (std::filesystem::exists(store.turns_path(job_id_it->second))) {
        const auto existing_turns = store.load_turns(job_id_it->second, &error);
        if (!existing_turns.has_value()) {
            std::cerr << "autodev run-task failed: " << error << '\n';
            return 1;
        }
        existing_turn_count = existing_turns->size();
    }

    std::cout << "AutoDev single-task pipeline\n"
              << "job_id:        " << job_id_it->second << '\n'
              << "stage:         execute\n";
    const int execute_code = RunExecuteNextTask(workspace, argc, argv);
    if (execute_code != 0) {
        std::cout << "stopped_stage: execute\n";
        return execute_code;
    }

    const auto turns = store.load_turns(job_id_it->second, &error);
    if (!turns.has_value()) {
        std::cerr << "autodev run-task failed: " << error << '\n';
        return 1;
    }
    if (turns->size() <= existing_turn_count) {
        std::cerr << "autodev run-task failed: execute stage did not record a new turn\n";
        return 1;
    }
    const auto& turn = turns->back();
    const auto task_id = turn.task_id;
    std::cout << "\nstage:         verify-task\n";
    const auto verification = store.verify_task(job_id_it->second, task_id, turn.turn_id);
    if (!verification.success) {
        std::cerr << "autodev run-task failed: " << verification.error_message << '\n';
        return 1;
    }
    std::cout << "verification_id: " << verification.verification.verification_id << '\n'
              << "verification_passed: " << (verification.verification.passed ? "true" : "false") << '\n';
    if (!verification.verification.passed) {
        std::cout << "stopped_stage: verify-task\n";
        PrintPipelineRepairHint(store, job_id_it->second, task_id);
        return 1;
    }

    std::cout << "\nstage:         diff-guard\n";
    const auto diff = store.diff_guard(job_id_it->second, task_id);
    if (!diff.success) {
        std::cerr << "autodev run-task failed: " << diff.error_message << '\n';
        return 1;
    }
    std::cout << "diff_id:       " << diff.diff_guard.diff_id << '\n'
              << "diff_passed:   " << (diff.diff_guard.passed ? "true" : "false") << '\n'
              << "changed_files: " << diff.diff_guard.changed_files.size() << '\n';
    if (!diff.diff_guard.passed) {
        std::cout << "stopped_stage: diff-guard\n";
        PrintPipelineRepairHint(store, job_id_it->second, task_id);
        return 1;
    }

    std::cout << "\nstage:         acceptance-gate\n";
    const auto acceptance = store.acceptance_gate(job_id_it->second, task_id);
    if (!acceptance.success) {
        std::cerr << "autodev run-task failed: " << acceptance.error_message << '\n';
        return 1;
    }
    std::cout << "acceptance_id: " << acceptance.acceptance.acceptance_id << '\n'
              << "acceptance_passed: " << (acceptance.acceptance.passed ? "true" : "false") << '\n'
              << "task_status:   " << acceptance.task.status << '\n'
              << "job_phase:     " << acceptance.job.phase << '\n'
              << "next_action:   " << acceptance.job.next_action << '\n';
    if (!acceptance.acceptance.passed) {
        std::cout << "stopped_stage: acceptance-gate\n";
        PrintPipelineRepairHint(store, job_id_it->second, task_id);
        return 1;
    }

    std::cout << "pipeline_status: passed\n"
              << "\nJob completion remains controlled by final-review and complete-job.\n";
    return 0;
}

int RunJobLoop(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev run-job failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev run-job failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::cout << "AutoDev job run loop\n"
              << "job_id:        " << job_id_it->second << '\n';
    int iterations = 0;
    while (true) {
        std::string error;
        const auto job = store.load_job(job_id_it->second, &error);
        if (!job.has_value()) {
            std::cerr << "autodev run-job failed: " << error << '\n';
            return 1;
        }
        const auto tasks = store.load_tasks(job_id_it->second, &error);
        if (!tasks.has_value()) {
            std::cerr << "autodev run-job failed: " << error << '\n';
            return 1;
        }
        const auto passed = std::count_if(tasks->begin(), tasks->end(), [](const AutoDevTask& task) {
            return task.status == "passed";
        });
        const auto pending = std::count_if(tasks->begin(), tasks->end(), [](const AutoDevTask& task) {
            return task.status == "pending";
        });

        std::cout << "loop_status:   status=" << job->status
                  << " phase=" << job->phase
                  << " tasks=" << passed << "/" << tasks->size()
                  << " pending=" << pending << '\n';
        if (job->phase == "final_review") {
            std::cout << "run_job_status: ready_for_final_review\n"
                      << "iterations:    " << iterations << '\n'
                      << "\nJob was not marked done. Next: agentos autodev final-review job_id="
                      << job_id_it->second << '\n';
            return 0;
        }
        if (job->status != "running" || job->phase != "codex_execution") {
            std::cerr << "autodev run-job failed: job is not runnable\n"
                      << "status: " << job->status << '\n'
                      << "phase:  " << job->phase << '\n';
            return 1;
        }
        if (pending == 0) {
            std::cerr << "autodev run-job failed: no pending tasks remain but job is not in final_review\n";
            return 1;
        }

        ++iterations;
        std::cout << "\niteration:     " << iterations << '\n';
        const int task_code = RunTaskPipeline(workspace, argc, argv);
        if (task_code != 0) {
            std::cout << "run_job_status: stopped\n"
                      << "iterations:    " << iterations << '\n';
            return task_code;
        }
    }
}

int RunStatus(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev status failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev status failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << error << '\n';
        return 1;
    }

    const auto watch_requested = options.find("watch") != options.end() ||
        (argc >= 3 && std::string(argv[2]) == "watch");
    if (watch_requested) {
        const auto iterations = std::max(1, ParseIntOption(options, "iterations", 1));
        const auto interval_ms = std::max(0, ParseIntOption(options, "interval_ms", 1000));
        std::cout << "AutoDev watch\n"
                  << "job_id:      " << job_id_it->second << '\n'
                  << "iterations:  " << iterations << '\n'
                  << "interval_ms: " << interval_ms << '\n';
        for (int iteration = 1; iteration <= iterations; ++iteration) {
            const auto watched_job = store.load_job(job_id_it->second, &error);
            if (!watched_job.has_value()) {
                std::cerr << "autodev watch failed: " << error << '\n';
                return 1;
            }
            const auto event_lines = store.load_event_lines(job_id_it->second, nullptr)
                .value_or(std::vector<std::string>{});
            std::string latest_event = "(none)";
            if (!event_lines.empty()) {
                try {
                    const auto event = nlohmann::json::parse(event_lines.back());
                    latest_event = event.value("type", "unknown");
                    if (event.contains("at") && event.at("at").is_string()) {
                        latest_event += " at " + event.at("at").get<std::string>();
                    }
                } catch (...) {
                    latest_event = "unparseable";
                }
            }
            std::cout << '\n'
                      << "Tick " << iteration << "/" << iterations << '\n'
                      << "  status:      " << watched_job->status << '\n'
                      << "  phase:       " << watched_job->phase << '\n'
                      << "  next_action: " << watched_job->next_action << '\n'
                      << "  events:      " << event_lines.size() << '\n'
                      << "  latest:      " << latest_event << '\n';
            if (iteration < iterations && interval_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            }
        }
        return 0;
    }

    std::cout << "Job: " << job->job_id << '\n'
              << "Status: " << job->status << '\n'
              << "Phase: " << job->phase << '\n'
              << "Current activity: " << job->current_activity << '\n'
              << "Approval gate: " << job->approval_gate << '\n'
              << '\n'
              << "Paths:\n"
              << "  agentos_workspace: " << job->agentos_workspace.string() << '\n'
              << "  target_repo_path:  " << job->target_repo_path.string() << '\n'
              << "  job_worktree_path: " << job->job_worktree_path.string()
              << (job->isolation_status == "ready" ? "" : " (planned)") << '\n'
              << '\n'
              << "Isolation:\n"
              << "  mode:              " << job->isolation_mode << '\n'
              << "  status:            " << job->isolation_status << '\n'
              << "  allow_dirty_target:" << (job->allow_dirty_target ? " true" : " false") << '\n'
              << "  head_sha:          " << job->created_from_head_sha.value_or("(none)") << '\n'
              << "  worktree_created:  " << job->worktree_created_at.value_or("(none)") << '\n'
              << '\n'
              << "Skill Pack:\n"
              << "  status: " << job->skill_pack.status << '\n';
    if (job->skill_pack.local_path.has_value()) {
        std::cout << "  path:   " << job->skill_pack.local_path->string() << '\n';
    }
    if (job->skill_pack.manifest_hash.has_value()) {
        std::cout << "  hash:   " << *job->skill_pack.manifest_hash << '\n';
    }
    if (job->skill_pack.error.has_value()) {
        std::cout << "  error:  " << *job->skill_pack.error << '\n';
    }
    if (job->spec_revision.has_value()) {
        std::cout << '\n'
                  << "Spec:\n"
                  << "  schema_version: " << job->schema_version.value_or("(none)") << '\n'
                  << "  revision:       " << *job->spec_revision << '\n'
                  << "  hash:           " << job->spec_hash.value_or("(none)") << '\n';
    }
    if (std::filesystem::exists(store.tasks_path(job->job_id))) {
        try {
            const auto tasks = store.load_tasks(job->job_id, &error);
            if (!tasks.has_value()) {
                throw std::runtime_error(error);
            }
            std::size_t passed = 0;
            for (const auto& task : *tasks) {
                if (task.status == "passed") {
                    ++passed;
                }
            }
            const auto progress = ComputeProgress(*job, *tasks);
            std::cout << '\n'
                      << "Tasks:\n"
                      << "  total:  " << tasks->size() << '\n'
                      << "  passed: " << passed << '\n'
                      << '\n'
                      << "Progress:\n";
            PrintProgress("  ", progress);
        } catch (...) {
            std::cout << '\n'
                      << "Tasks:\n"
                      << "  error: failed to read tasks.json\n";
        }
    }
    std::cout << '\n'
              << "Next action:\n";
    if (job->next_action == "none") {
        std::cout << "  none";
    } else {
        std::cout << "  " << AutoDevCommandForNextAction(*job);
    }
    std::cout << '\n'
              << '\n';
    if (job->blocker.has_value()) {
        std::cout << "Blocker:\n"
                  << "  " << *job->blocker << '\n'
                  << '\n';
    }
    const auto recovery_hint = AutoDevRecoveryHint(*job);
    if (!recovery_hint.empty()) {
        std::cout << "Recovery:\n"
                  << "  " << recovery_hint << '\n'
                  << '\n';
    }
    if (job->isolation_status == "ready") {
        std::cout << "Workspace is ready. Future AutoDev writes must use the job worktree.\n";
    } else {
        std::cout << "Workspace is not ready yet. No target files have been modified.\n";
    }
    return 0;
}

}  // namespace

int RunAutoDevCommand(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }
    const std::string subcommand = argv[2];
    std::unique_ptr<AutoDevJobRuntimeLock> job_runtime_lock;
    if (AutoDevSubcommandNeedsJobRuntimeLock(subcommand)) {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto job_id_it = options.find("job_id");
        if (job_id_it != options.end() && IsValidAutoDevJobId(job_id_it->second)) {
            try {
                AutoDevStateStore store(workspace);
                job_runtime_lock = std::make_unique<AutoDevJobRuntimeLock>(
                    store.job_dir(job_id_it->second) / "job.lock");
            } catch (const std::exception& e) {
                std::cerr << "autodev " << subcommand << " failed: " << e.what() << '\n';
                return 1;
            }
        }
    }
    if (subcommand == "submit") {
        return RunSubmit(workspace, argc, argv);
    }
    if (subcommand == "status" || subcommand == "watch") {
        return RunStatus(workspace, argc, argv);
    }
    if (subcommand == "summary") {
        return RunSummary(workspace, argc, argv);
    }
    if (subcommand == "prepare-workspace" || subcommand == "prepare_workspace") {
        return RunPrepareWorkspace(workspace, argc, argv);
    }
    if (subcommand == "load-skill-pack" || subcommand == "load_skill_pack") {
        return RunLoadSkillPack(workspace, argc, argv);
    }
    if (subcommand == "generate-goal-docs" || subcommand == "generate_goal_docs") {
        return RunGenerateGoalDocs(workspace, argc, argv);
    }
    if (subcommand == "validate-spec" || subcommand == "validate_spec") {
        return RunValidateSpec(workspace, argc, argv);
    }
    if (subcommand == "approve-spec" || subcommand == "approve_spec") {
        return RunApproveSpec(workspace, argc, argv);
    }
    if (subcommand == "recover-blocked" || subcommand == "recover_blocked") {
        return RunRecoverBlocked(workspace, argc, argv);
    }
    if (subcommand == "recover-crash" || subcommand == "recover_crash") {
        return RunRecoverCrash(workspace, argc, argv);
    }
    if (subcommand == "tasks") {
        return RunTasks(workspace, argc, argv);
    }
    if (subcommand == "turns") {
        return RunTurns(workspace, argc, argv);
    }
    if (subcommand == "snapshot-task" || subcommand == "snapshot_task") {
        return RunSnapshotTask(workspace, argc, argv);
    }
    if (subcommand == "snapshots") {
        return RunSnapshots(workspace, argc, argv);
    }
    if (subcommand == "rollback-soft" || subcommand == "rollback_soft") {
        return RunRollbackSoft(workspace, argc, argv);
    }
    if (subcommand == "rollback-hard" || subcommand == "rollback_hard") {
        return RunRollbackHard(workspace, argc, argv);
    }
    if (subcommand == "rollbacks") {
        return RunRollbacks(workspace, argc, argv);
    }
    if (subcommand == "repairs") {
        return RunRepairs(workspace, argc, argv);
    }
    if (subcommand == "repair-next" || subcommand == "repair_next") {
        return RunRepairNextOrTask(workspace, argc, argv, false);
    }
    if (subcommand == "repair-task" || subcommand == "repair_task") {
        return RunRepairNextOrTask(workspace, argc, argv, true);
    }
    if (subcommand == "verify-task" || subcommand == "verify_task") {
        return RunVerifyTask(workspace, argc, argv);
    }
    if (subcommand == "verifications") {
        return RunVerifications(workspace, argc, argv);
    }
    if (subcommand == "diff-guard" || subcommand == "diff_guard") {
        return RunDiffGuard(workspace, argc, argv);
    }
    if (subcommand == "diffs") {
        return RunDiffs(workspace, argc, argv);
    }
    if (subcommand == "acceptance-gate" || subcommand == "acceptance_gate") {
        return RunAcceptanceGate(workspace, argc, argv);
    }
    if (subcommand == "acceptances") {
        return RunAcceptances(workspace, argc, argv);
    }
    if (subcommand == "final-review" || subcommand == "final_review") {
        return RunFinalReview(workspace, argc, argv);
    }
    if (subcommand == "final-reviews" || subcommand == "final_reviews") {
        return RunFinalReviews(workspace, argc, argv);
    }
    if (subcommand == "complete-job" || subcommand == "complete_job" ||
        subcommand == "mark-done" || subcommand == "mark_done") {
        return RunCompleteJob(workspace, argc, argv);
    }
    if (subcommand == "pause") {
        return RunJobControl(workspace, argc, argv, "pause", &AutoDevStateStore::pause_job);
    }
    if (subcommand == "resume") {
        return RunJobControl(workspace, argc, argv, "resume", &AutoDevStateStore::resume_job);
    }
    if (subcommand == "cancel") {
        return RunJobControl(workspace, argc, argv, "cancel", &AutoDevStateStore::cancel_job);
    }
    if (subcommand == "cleanup-worktree" || subcommand == "cleanup_worktree") {
        return RunCleanupWorktree(workspace, argc, argv);
    }
    if (subcommand == "pr-summary" || subcommand == "pr_summary") {
        return RunPrSummary(workspace, argc, argv);
    }
    if (subcommand == "events") {
        return RunEvents(workspace, argc, argv);
    }
    if (subcommand == "execute-next-task" || subcommand == "execute_next_task") {
        return RunExecuteNextTask(workspace, argc, argv);
    }
    if (subcommand == "run-job" || subcommand == "run_job") {
        return RunJobLoop(workspace, argc, argv);
    }
    if (subcommand == "run-task" || subcommand == "run_task" ||
        subcommand == "pipeline-task" || subcommand == "pipeline_task") {
        return RunTaskPipeline(workspace, argc, argv);
    }

    std::cerr << "Unknown autodev subcommand: " << subcommand << '\n';
    PrintUsage();
    return 1;
}

}  // namespace agentos
