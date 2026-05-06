#include "autodev/autodev_state_store.hpp"

#include "autodev/autodev_job_id.hpp"
#include "utils/atomic_file.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace agentos {

namespace {

std::string IsoUtcNow() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::filesystem::path AbsoluteNormalized(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::path PlannedWorktreePath(
    const std::filesystem::path& target_repo_path,
    const std::string& job_id) {
    const auto parent = target_repo_path.parent_path();
    const auto name = target_repo_path.filename().string();
    return parent / (name + "-autodev-" + AutoDevJobIdSuffix(job_id));
}

nlohmann::json SubmitEvent(const AutoDevJob& job) {
    return nlohmann::json{
        {"type", "autodev.job.submitted"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"isolation_mode", job.isolation_mode},
        {"isolation_status", job.isolation_status},
        {"target_repo_path", job.target_repo_path.string()},
        {"planned_worktree_path", job.job_worktree_path.string()},
        {"next_action", job.next_action},
        {"at", job.created_at},
    };
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

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult RunCommand(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) {
        return {.exit_code = -1, .output = "failed to start command"};
    }
    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#ifdef _WIN32
    const int status = _pclose(pipe);
    return {.exit_code = status, .output = std::move(output)};
#else
    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        return {.exit_code = WEXITSTATUS(status), .output = std::move(output)};
    }
    return {.exit_code = status, .output = std::move(output)};
#endif
}

CommandResult GitCommand(const std::filesystem::path& repo, const std::string& args) {
    return RunCommand("git -C " + ShellQuote(repo.string()) + " " + args);
}

std::string Trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

nlohmann::json WorkspacePreparedEvent(const AutoDevJob& job) {
    return nlohmann::json{
        {"type", "autodev.workspace.prepared"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"isolation_mode", job.isolation_mode},
        {"isolation_status", job.isolation_status},
        {"target_repo_path", job.target_repo_path.string()},
        {"job_worktree_path", job.job_worktree_path.string()},
        {"created_from_head_sha", job.created_from_head_sha.value_or("")},
        {"next_action", job.next_action},
        {"at", job.updated_at},
    };
}

nlohmann::json WorkspaceBlockedEvent(const AutoDevJob& job) {
    return nlohmann::json{
        {"type", "autodev.workspace.blocked"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"isolation_mode", job.isolation_mode},
        {"isolation_status", job.isolation_status},
        {"blocker", job.blocker.value_or("")},
        {"next_action", job.next_action},
        {"at", job.updated_at},
    };
}

}  // namespace

AutoDevStateStore::AutoDevStateStore(std::filesystem::path agentos_workspace)
    : agentos_workspace_(AbsoluteNormalized(agentos_workspace)) {}

const std::filesystem::path& AutoDevStateStore::agentos_workspace() const {
    return agentos_workspace_;
}

std::filesystem::path AutoDevStateStore::root_dir() const {
    return agentos_workspace_ / "runtime" / "autodev";
}

std::filesystem::path AutoDevStateStore::jobs_dir() const {
    return root_dir() / "jobs";
}

std::filesystem::path AutoDevStateStore::job_dir(const std::string& job_id) const {
    return jobs_dir() / job_id;
}

std::filesystem::path AutoDevStateStore::job_json_path(const std::string& job_id) const {
    return job_dir(job_id) / "job.json";
}

std::filesystem::path AutoDevStateStore::events_path(const std::string& job_id) const {
    return job_dir(job_id) / "events.ndjson";
}

AutoDevSubmitResult AutoDevStateStore::submit(const AutoDevSubmitRequest& request) {
    if (request.target_repo_path.empty()) {
        AutoDevSubmitResult result;
        result.error_message = "target_repo_path is required";
        return result;
    }
    if (request.objective.empty()) {
        AutoDevSubmitResult result;
        result.error_message = "objective is required";
        return result;
    }
    if (request.isolation_mode != "git_worktree" && request.isolation_mode != "in_place") {
        AutoDevSubmitResult result;
        result.error_message = "isolation_mode must be git_worktree or in_place";
        return result;
    }

    const auto target_path = AbsoluteNormalized(request.target_repo_path);
    std::error_code ec;
    if (!std::filesystem::exists(target_path, ec)) {
        AutoDevSubmitResult result;
        result.error_message = "target_repo_path does not exist: " + target_path.string();
        return result;
    }

    std::filesystem::create_directories(jobs_dir());

    AutoDevJob job;
    for (int attempt = 0; attempt < 32; ++attempt) {
        job.job_id = GenerateAutoDevJobId();
        if (!std::filesystem::exists(job_dir(job.job_id))) {
            break;
        }
        job.job_id.clear();
    }
    if (job.job_id.empty()) {
        AutoDevSubmitResult result;
        result.error_message = "could not allocate unique AutoDev job id";
        return result;
    }

    const auto timestamp = IsoUtcNow();
    job.agentos_workspace = request.agentos_workspace.empty()
        ? agentos_workspace_
        : AbsoluteNormalized(request.agentos_workspace);
    job.target_repo_path = target_path;
    job.job_worktree_path = PlannedWorktreePath(target_path, job.job_id);
    job.isolation_mode = request.isolation_mode;
    job.isolation_status = "pending";
    job.allow_dirty_target = request.allow_dirty_target;
    job.created_at = timestamp;
    job.updated_at = timestamp;

    if (request.skill_pack_path.has_value() && !request.skill_pack_path->empty()) {
        job.skill_pack.source_type = "local_path";
        job.skill_pack.local_path = AbsoluteNormalized(*request.skill_pack_path);
        job.skill_pack.status = "declared";
    }

    const auto dir = job_dir(job.job_id);
    std::filesystem::create_directories(dir);
    save_job(job);
    append_event(job.job_id, SubmitEvent(job));

    AutoDevSubmitResult result;
    result.success = true;
    result.job = std::move(job);
    result.job_dir = dir;
    return result;
}

void AutoDevStateStore::save_job(const AutoDevJob& job) const {
    WriteFileAtomically(job_json_path(job.job_id), ToJson(job).dump(2) + "\n");
}

void AutoDevStateStore::append_event(const std::string& job_id, const nlohmann::json& event) const {
    AppendLineToFile(events_path(job_id), event.dump());
}

AutoDevPrepareWorkspaceResult AutoDevStateStore::prepare_workspace(const std::string& job_id) {
    std::string load_error;
    auto maybe_job = load_job(job_id, &load_error);
    if (!maybe_job.has_value()) {
        AutoDevPrepareWorkspaceResult result;
        result.error_message = load_error;
        return result;
    }

    AutoDevJob job = std::move(*maybe_job);
    if (job.isolation_status == "ready") {
        AutoDevPrepareWorkspaceResult result;
        result.success = true;
        result.job = std::move(job);
        return result;
    }
    if (job.isolation_mode != "git_worktree") {
        job.status = "blocked";
        job.phase = "workspace_preparing";
        job.current_activity = "none";
        job.isolation_status = "blocked";
        job.blocker = "prepare_workspace only supports git_worktree isolation in this slice";
        job.next_action = "fix_workspace_or_use_git_worktree";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, WorkspaceBlockedEvent(job));

        AutoDevPrepareWorkspaceResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }

    job.status = "running";
    job.phase = "workspace_preparing";
    job.current_activity = "creating_worktree";
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, nlohmann::json{
        {"type", "autodev.workspace.preparing"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"current_activity", job.current_activity},
        {"at", job.updated_at},
    });

    const auto git_root = GitCommand(job.target_repo_path, "rev-parse --show-toplevel");
    if (git_root.exit_code != 0) {
        job.status = "blocked";
        job.current_activity = "none";
        job.isolation_status = "blocked";
        job.blocker = "target_repo_path is not a git repository";
        job.next_action = "fix_workspace_or_use_in_place";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, WorkspaceBlockedEvent(job));

        AutoDevPrepareWorkspaceResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }

    const auto dirty = GitCommand(job.target_repo_path, "status --porcelain");
    if (dirty.exit_code != 0 || (!Trim(dirty.output).empty() && !job.allow_dirty_target)) {
        job.status = "blocked";
        job.current_activity = "none";
        job.isolation_status = "blocked";
        job.blocker = dirty.exit_code != 0
            ? "could not inspect target git status"
            : "target_repo_path has uncommitted changes";
        job.next_action = "commit_or_allow_dirty_target";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, WorkspaceBlockedEvent(job));

        AutoDevPrepareWorkspaceResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }

    const auto head = GitCommand(job.target_repo_path, "rev-parse HEAD");
    if (head.exit_code != 0) {
        job.status = "blocked";
        job.current_activity = "none";
        job.isolation_status = "blocked";
        job.blocker = "could not resolve target HEAD";
        job.next_action = "fix_workspace_or_use_in_place";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, WorkspaceBlockedEvent(job));

        AutoDevPrepareWorkspaceResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }

    auto worktree_path = job.job_worktree_path;
    for (int attempt = 1; std::filesystem::exists(worktree_path) && attempt < 32; ++attempt) {
        worktree_path = job.job_worktree_path;
        worktree_path += "-" + std::to_string(attempt);
    }
    if (std::filesystem::exists(worktree_path)) {
        job.status = "blocked";
        job.current_activity = "none";
        job.isolation_status = "blocked";
        job.blocker = "could not choose a free job_worktree_path";
        job.next_action = "fix_workspace_path";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, WorkspaceBlockedEvent(job));

        AutoDevPrepareWorkspaceResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }
    job.job_worktree_path = worktree_path;

    const auto add = GitCommand(
        job.target_repo_path,
        "worktree add --detach " + ShellQuote(job.job_worktree_path.string()) + " HEAD");
    if (add.exit_code != 0) {
        job.status = "blocked";
        job.current_activity = "none";
        job.isolation_status = "blocked";
        job.blocker = "Git worktree could not be created: " + Trim(add.output);
        job.next_action = "fix_workspace_or_use_in_place";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, WorkspaceBlockedEvent(job));

        AutoDevPrepareWorkspaceResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }

    job.status = "running";
    job.phase = "system_understanding";
    job.current_activity = "none";
    job.isolation_status = "ready";
    job.created_from_head_sha = Trim(head.output);
    job.worktree_created_at = IsoUtcNow();
    job.next_action = job.skill_pack.status == "declared" ? "load_skill_pack" : "declare_skill_pack";
    job.blocker = std::nullopt;
    job.updated_at = *job.worktree_created_at;
    save_job(job);
    append_event(job.job_id, WorkspacePreparedEvent(job));

    AutoDevPrepareWorkspaceResult result;
    result.success = true;
    result.job = std::move(job);
    return result;
}

std::optional<AutoDevJob> AutoDevStateStore::load_job(
    const std::string& job_id,
    std::string* error_message) const {
    if (!IsValidAutoDevJobId(job_id)) {
        if (error_message) {
            *error_message = "invalid AutoDev job_id: " + job_id;
        }
        return std::nullopt;
    }

    const auto path = job_json_path(job_id);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error_message) {
            *error_message = "AutoDev job not found: " + job_id + "\nExpected path:\n" + path.string();
        }
        return std::nullopt;
    }

    try {
        nlohmann::json json;
        input >> json;
        return AutoDevJobFromJson(json);
    } catch (const std::exception& e) {
        if (error_message) {
            *error_message = std::string("failed to read AutoDev job: ") + e.what();
        }
        return std::nullopt;
    }
}

}  // namespace agentos
