#include "autodev/autodev_state_store.hpp"

#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_skill_pack_loader.hpp"
#include "utils/atomic_file.hpp"
#include "utils/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
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

nlohmann::json SkillPackLoadedEvent(const AutoDevJob& job, const std::filesystem::path& snapshot_path) {
    return nlohmann::json{
        {"type", "autodev.skill_pack.loaded"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"skill_pack_status", job.skill_pack.status},
        {"skill_pack_path", job.skill_pack.local_path.has_value() ? job.skill_pack.local_path->string() : ""},
        {"manifest_hash", job.skill_pack.manifest_hash.value_or("")},
        {"snapshot_path", snapshot_path.string()},
        {"next_action", job.next_action},
        {"at", job.updated_at},
    };
}

nlohmann::json SkillPackBlockedEvent(const AutoDevJob& job) {
    return nlohmann::json{
        {"type", "autodev.skill_pack.blocked"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"skill_pack_status", job.skill_pack.status},
        {"blocker", job.blocker.value_or("")},
        {"next_action", job.next_action},
        {"at", job.updated_at},
    };
}

nlohmann::json GoalDocsGeneratedEvent(const AutoDevJob& job, const std::vector<std::filesystem::path>& written_files) {
    nlohmann::json files = nlohmann::json::array();
    std::error_code ec;
    for (const auto& file : written_files) {
        files.push_back(std::filesystem::relative(file, job.job_worktree_path, ec).generic_string());
    }
    return nlohmann::json{
        {"type", "autodev.goal_docs.generated"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"goal_dir", (job.job_worktree_path / "docs" / "goal").string()},
        {"files", files},
        {"next_action", job.next_action},
        {"at", job.updated_at},
    };
}

nlohmann::json SpecValidatedEvent(const AutoDevJob& job) {
    return nlohmann::json{
        {"type", "autodev.spec.validated"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"approval_gate", job.approval_gate},
        {"schema_version", job.schema_version.value_or("")},
        {"spec_revision", job.spec_revision.value_or("")},
        {"spec_hash", job.spec_hash.value_or("")},
        {"next_action", job.next_action},
        {"at", job.updated_at},
    };
}

std::string NextSpecRevision(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    int max_revision = 0;
    const std::regex pattern(R"(rev-([0-9]{3})\.normalized\.json)");
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        std::smatch match;
        const auto name = entry.path().filename().string();
        if (std::regex_match(name, match, pattern)) {
            max_revision = std::max(max_revision, std::stoi(match[1].str()));
        }
    }

    std::ostringstream out;
    out << "rev-" << std::setw(3) << std::setfill('0') << (max_revision + 1);
    return out.str();
}

std::string ValidateAutoDevSpecShape(const nlohmann::json& spec) {
    if (!spec.is_object()) {
        return "AUTODEV_SPEC.json must contain a JSON object";
    }
    const std::vector<std::pair<const char*, nlohmann::json::value_t>> required = {
        {"schema_version", nlohmann::json::value_t::string},
        {"generated_by", nlohmann::json::value_t::string},
        {"generated_by_skill_pack", nlohmann::json::value_t::string},
        {"agentos_min_version", nlohmann::json::value_t::string},
        {"created_at", nlohmann::json::value_t::string},
        {"objective", nlohmann::json::value_t::string},
        {"mode", nlohmann::json::value_t::string},
        {"source_of_truth", nlohmann::json::value_t::array},
        {"tasks", nlohmann::json::value_t::array},
    };
    for (const auto& [key, type] : required) {
        if (!spec.contains(key)) {
            return std::string("AUTODEV_SPEC.json missing required field: ") + key;
        }
        if (spec.at(key).type() != type) {
            return std::string("AUTODEV_SPEC.json field has invalid type: ") + key;
        }
    }
    if (spec.at("schema_version").get<std::string>() != "1.0.0") {
        return "unsupported AUTODEV_SPEC schema_version: " + spec.at("schema_version").get<std::string>();
    }
    return {};
}

std::string MarkdownSkeleton(const AutoDevJob& job, const std::string& title, const std::string& body) {
    std::ostringstream out;
    out << "# " << title << "\n\n"
        << "> Candidate AutoDev working document generated by AgentOS.\n"
        << "> This file is not a frozen spec and is not AcceptanceGate authority.\n\n"
        << "Job ID: `" << job.job_id << "`\n\n"
        << "Objective:\n\n"
        << "```text\n" << job.objective << "\n```\n\n"
        << body << "\n";
    return out.str();
}

std::string GoalContractSkeleton(const AutoDevJob& job) {
    std::ostringstream out;
    out << "# GOAL\n\n"
        << "> Candidate AutoDev goal contract generated by AgentOS.\n"
        << "> This is working context only until AgentOS validates, snapshots, and freezes an AutoDev spec revision.\n\n"
        << "Job ID: `" << job.job_id << "`\n\n"
        << "Objective:\n\n"
        << "```text\n" << job.objective << "\n```\n\n"
        << "## Source Documents\n\n"
        << "- `REQUIREMENTS.md`\n"
        << "- `DESIGN.md`\n"
        << "- `NON_GOALS.md`\n"
        << "- `IMPACT.md`\n"
        << "- `TASKS.md`\n"
        << "- `ACCEPTANCE.md`\n"
        << "- `AUTODEV_SPEC.json`\n\n"
        << "## Status\n\n"
        << "Candidate only. Not frozen.\n";
    return out.str();
}

std::string CodexStartSkeleton(const AutoDevJob& job) {
    std::ostringstream out;
    out << "# CODEX_START\n\n"
        << "Do not execute code from this candidate context until AgentOS has created an approved Frozen Spec.\n\n"
        << "Job ID: `" << job.job_id << "`\n\n"
        << "Objective:\n\n"
        << "```text\n" << job.objective << "\n```\n\n"
        << "Rules:\n\n"
        << "- Do not expand scope.\n"
        << "- Do not edit `AUTODEV_SPEC.json` to pass validation.\n"
        << "- Do not self-certify task or job completion.\n"
        << "- AgentOS AcceptanceGate is the completion authority.\n";
    return out.str();
}

std::string AutoDevSpecSkeleton(const AutoDevJob& job) {
    const auto spec = nlohmann::json{
        {"schema_version", "1.0.0"},
        {"generated_by", "agentos"},
        {"generated_by_skill_pack", job.skill_pack.name.value_or("maxenergy/skills")},
        {"skill_pack_version", nullptr},
        {"skill_pack_commit", job.skill_pack.commit.has_value() ? nlohmann::json(*job.skill_pack.commit) : nlohmann::json(nullptr)},
        {"agentos_min_version", "0.1.0"},
        {"created_at", IsoUtcNow()},
        {"objective", job.objective},
        {"mode", "feature"},
        {"source_of_truth", nlohmann::json::array({
            "docs/goal/REQUIREMENTS.md",
            "docs/goal/DESIGN.md",
            "docs/goal/NON_GOALS.md",
            "docs/goal/IMPACT.md",
            "docs/goal/TASKS.md",
            "docs/goal/ACCEPTANCE.md"
        })},
        {"tasks", nlohmann::json::array()},
        {"status", "candidate_skeleton"}
    };
    return spec.dump(2) + "\n";
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

std::filesystem::path AutoDevStateStore::artifacts_dir(const std::string& job_id) const {
    return job_dir(job_id) / "artifacts";
}

std::filesystem::path AutoDevStateStore::spec_revisions_dir(const std::string& job_id) const {
    return job_dir(job_id) / "spec_revisions";
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
    job.objective = request.objective;
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

AutoDevLoadSkillPackResult AutoDevStateStore::load_skill_pack(
    const std::string& job_id,
    const std::optional<std::filesystem::path>& override_path) {
    std::string load_error;
    auto maybe_job = load_job(job_id, &load_error);
    if (!maybe_job.has_value()) {
        AutoDevLoadSkillPackResult result;
        result.error_message = load_error;
        return result;
    }

    AutoDevJob job = std::move(*maybe_job);
    if (override_path.has_value() && !override_path->empty()) {
        job.skill_pack.source_type = "local_path";
        job.skill_pack.local_path = AbsoluteNormalized(*override_path);
        job.skill_pack.status = "declared";
        job.skill_pack.error = std::nullopt;
    }
    if (!job.skill_pack.local_path.has_value() || job.skill_pack.local_path->empty()) {
        job.status = "blocked";
        job.current_activity = "none";
        job.skill_pack.status = "missing";
        job.skill_pack.error = "skill_pack_path is required";
        job.blocker = "skill_pack_path is required";
        job.next_action = "declare_skill_pack";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, SkillPackBlockedEvent(job));

        AutoDevLoadSkillPackResult result;
        result.error_message = *job.blocker;
        result.job = std::move(job);
        return result;
    }

    job.status = "running";
    job.current_activity = "loading_skill_pack";
    job.skill_pack.status = "loading";
    job.skill_pack.error = std::nullopt;
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, nlohmann::json{
        {"type", "autodev.skill_pack.loading"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"skill_pack_path", job.skill_pack.local_path->string()},
        {"at", job.updated_at},
    });

    const AutoDevSkillPackLoader loader;
    const auto loaded = loader.load_local_path(*job.skill_pack.local_path);
    if (!loaded.success) {
        job.status = "blocked";
        job.current_activity = "none";
        job.skill_pack.status = loaded.error_message.find("does not exist") != std::string::npos
            ? "missing"
            : "validation_failed";
        job.skill_pack.error = loaded.error_message;
        job.blocker = loaded.error_message;
        job.next_action = "fix_skill_pack";
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, SkillPackBlockedEvent(job));

        AutoDevLoadSkillPackResult result;
        result.error_message = loaded.error_message;
        result.job = std::move(job);
        return result;
    }

    const auto snapshot_dir = artifacts_dir(job.job_id);
    std::filesystem::create_directories(snapshot_dir);
    const auto snapshot_path = snapshot_dir / "skill_pack.snapshot.json";
    WriteFileAtomically(snapshot_path, loaded.snapshot.dump(2) + "\n");

    job.status = job.isolation_status == "ready" ? "running" : "submitted";
    job.current_activity = "none";
    job.skill_pack.name = "maxenergy/skills";
    job.skill_pack.source_type = "local_path";
    job.skill_pack.commit = loaded.commit.empty() ? std::nullopt : std::optional<std::string>(loaded.commit);
    job.skill_pack.status = "loaded";
    job.skill_pack.loaded_at = IsoUtcNow();
    job.skill_pack.manifest_hash = loaded.manifest_hash;
    job.skill_pack.error = std::nullopt;
    job.blocker = std::nullopt;
    job.next_action = job.isolation_status == "ready" ? "generate_goal_docs" : "prepare_workspace";
    job.updated_at = *job.skill_pack.loaded_at;
    save_job(job);
    append_event(job.job_id, SkillPackLoadedEvent(job, snapshot_path));

    AutoDevLoadSkillPackResult result;
    result.success = true;
    result.job = std::move(job);
    result.snapshot_path = snapshot_path;
    return result;
}

AutoDevGenerateGoalDocsResult AutoDevStateStore::generate_goal_docs(const std::string& job_id) {
    std::string load_error;
    auto maybe_job = load_job(job_id, &load_error);
    if (!maybe_job.has_value()) {
        AutoDevGenerateGoalDocsResult result;
        result.error_message = load_error;
        return result;
    }

    AutoDevJob job = std::move(*maybe_job);
    const auto block = [this](AutoDevJob job, const std::string& message, const std::string& next_action) {
        job.status = "blocked";
        job.current_activity = "none";
        job.blocker = message;
        job.next_action = next_action;
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, nlohmann::json{
            {"type", "autodev.goal_docs.blocked"},
            {"job_id", job.job_id},
            {"status", job.status},
            {"phase", job.phase},
            {"blocker", message},
            {"next_action", next_action},
            {"at", job.updated_at},
        });
        AutoDevGenerateGoalDocsResult result;
        result.error_message = message;
        result.job = std::move(job);
        return result;
    };

    if (job.isolation_status != "ready" || !std::filesystem::exists(job.job_worktree_path)) {
        return block(std::move(job), "workspace is not ready", "prepare_workspace");
    }
    if (job.skill_pack.status != "loaded") {
        return block(std::move(job), "skill pack is not loaded", "load_skill_pack");
    }

    job.status = "running";
    job.current_activity = "generating_system_docs";
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, nlohmann::json{
        {"type", "autodev.goal_docs.generating"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"current_activity", job.current_activity},
        {"at", job.updated_at},
    });

    const auto goal_dir = job.job_worktree_path / "docs" / "goal";
    std::filesystem::create_directories(goal_dir);
    std::vector<std::pair<std::string, std::string>> files = {
        {"QUESTIONS.md", MarkdownSkeleton(job, "Questions", "## Blocking Questions\n\n- TODO\n\n## Non-Blocking Questions\n\n- TODO\n")},
        {"ANSWERS.md", MarkdownSkeleton(job, "Answers", "## Answers\n\n- TODO\n")},
        {"ASSUMPTIONS.md", MarkdownSkeleton(job, "Assumptions", "## Assumptions\n\n- TODO\n")},
        {"UNKNOWN.md", MarkdownSkeleton(job, "Unknowns", "## Blocking Unknowns\n\n- TODO\n\n## Non-Blocking Unknowns\n\n- TODO\n")},
        {"REQUIREMENTS.md", MarkdownSkeleton(job, "Requirements", "## Requirements\n\n- TODO\n")},
        {"DESIGN.md", MarkdownSkeleton(job, "Design", "## Design\n\n- TODO\n")},
        {"NON_GOALS.md", MarkdownSkeleton(job, "Non-Goals", "## Non-Goals\n\n- TODO\n")},
        {"IMPACT.md", MarkdownSkeleton(job, "Impact", "## Impact Analysis\n\n- TODO\n")},
        {"TASKS.md", MarkdownSkeleton(job, "Tasks", "## Tasks\n\n- TODO\n")},
        {"ACCEPTANCE.md", MarkdownSkeleton(job, "Acceptance", "## Acceptance Criteria\n\n- TODO\n")},
        {"GOAL.md", GoalContractSkeleton(job)},
        {"CODEX_START.md", CodexStartSkeleton(job)},
        {"AUTODEV_SPEC.json", AutoDevSpecSkeleton(job)},
    };

    std::vector<std::filesystem::path> written;
    for (const auto& [name, content] : files) {
        const auto path = goal_dir / name;
        WriteFileAtomically(path, content);
        written.push_back(path);
    }

    job.phase = "requirements_grilling";
    job.current_activity = "none";
    job.next_action = "validate_spec";
    job.blocker = std::nullopt;
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, GoalDocsGeneratedEvent(job, written));

    AutoDevGenerateGoalDocsResult result;
    result.success = true;
    result.job = std::move(job);
    result.goal_dir = goal_dir;
    result.written_files = std::move(written);
    return result;
}

AutoDevValidateSpecResult AutoDevStateStore::validate_spec(const std::string& job_id) {
    std::string load_error;
    auto maybe_job = load_job(job_id, &load_error);
    if (!maybe_job.has_value()) {
        AutoDevValidateSpecResult result;
        result.error_message = load_error;
        return result;
    }

    AutoDevJob job = std::move(*maybe_job);
    const auto block = [this](AutoDevJob job, const std::string& message, const std::string& next_action) {
        job.status = "blocked";
        job.current_activity = "none";
        job.blocker = message;
        job.next_action = next_action;
        job.updated_at = IsoUtcNow();
        save_job(job);
        append_event(job.job_id, nlohmann::json{
            {"type", "autodev.spec.blocked"},
            {"job_id", job.job_id},
            {"status", job.status},
            {"phase", job.phase},
            {"blocker", message},
            {"next_action", next_action},
            {"at", job.updated_at},
        });
        AutoDevValidateSpecResult result;
        result.error_message = message;
        result.job = std::move(job);
        return result;
    };

    if (job.isolation_status != "ready" || !std::filesystem::exists(job.job_worktree_path)) {
        return block(std::move(job), "workspace is not ready", "prepare_workspace");
    }
    const auto spec_path = job.job_worktree_path / "docs" / "goal" / "AUTODEV_SPEC.json";
    if (!std::filesystem::exists(spec_path)) {
        return block(std::move(job), "AUTODEV_SPEC.json does not exist in job worktree", "generate_goal_docs");
    }

    job.status = "running";
    job.phase = "spec_freezing";
    job.current_activity = "validating_spec";
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, nlohmann::json{
        {"type", "autodev.spec.validating"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"current_activity", job.current_activity},
        {"spec_path", spec_path.string()},
        {"at", job.updated_at},
    });

    nlohmann::json spec;
    try {
        std::ifstream input(spec_path, std::ios::binary);
        input >> spec;
    } catch (const std::exception& e) {
        return block(std::move(job), std::string("failed to parse AUTODEV_SPEC.json: ") + e.what(), "fix_autodev_spec");
    }

    const auto validation_error = ValidateAutoDevSpecShape(spec);
    if (!validation_error.empty()) {
        return block(std::move(job), validation_error, "fix_autodev_spec");
    }

    job.current_activity = "snapshotting_spec";
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, nlohmann::json{
        {"type", "autodev.spec.snapshotting"},
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"current_activity", job.current_activity},
        {"at", job.updated_at},
    });

    const auto revisions_dir = spec_revisions_dir(job.job_id);
    std::filesystem::create_directories(revisions_dir);
    const auto revision = NextSpecRevision(revisions_dir);
    const auto normalized = spec.dump(2) + "\n";
    const auto hash = Sha256Hex(normalized);
    const auto normalized_path = revisions_dir / (revision + ".normalized.json");
    const auto hash_path = revisions_dir / (revision + ".sha256");
    const auto status_path = revisions_dir / (revision + ".status.json");
    WriteFileAtomically(normalized_path, normalized);
    WriteFileAtomically(hash_path, hash + "\n");
    WriteFileAtomically(status_path, nlohmann::json{
        {"job_id", job.job_id},
        {"spec_revision", revision},
        {"spec_hash", hash},
        {"schema_version", spec.at("schema_version").get<std::string>()},
        {"status", "pending_approval"},
        {"approval_gate", "before_code_execution"},
        {"created_at", IsoUtcNow()},
    }.dump(2) + "\n");

    job.status = "awaiting_approval";
    job.phase = "goal_packing";
    job.current_activity = "none";
    job.approval_gate = "before_code_execution";
    job.schema_version = spec.at("schema_version").get<std::string>();
    job.spec_revision = revision;
    job.spec_hash = hash;
    job.next_action = "approve_spec";
    job.blocker = std::nullopt;
    job.updated_at = IsoUtcNow();
    save_job(job);
    append_event(job.job_id, SpecValidatedEvent(job));

    AutoDevValidateSpecResult result;
    result.success = true;
    result.job = std::move(job);
    result.spec_revision = revision;
    result.spec_hash = hash;
    result.normalized_path = normalized_path;
    result.hash_path = hash_path;
    result.status_path = status_path;
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
