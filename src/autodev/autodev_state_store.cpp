#include "autodev/autodev_state_store.hpp"

#include "autodev/autodev_job_id.hpp"
#include "utils/atomic_file.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

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
    job.created_at = timestamp;
    job.updated_at = timestamp;

    if (request.skill_pack_path.has_value() && !request.skill_pack_path->empty()) {
        job.skill_pack.source_type = "local_path";
        job.skill_pack.local_path = AbsoluteNormalized(*request.skill_pack_path);
        job.skill_pack.status = "declared";
    }

    const auto dir = job_dir(job.job_id);
    std::filesystem::create_directories(dir);
    WriteFileAtomically(job_json_path(job.job_id), ToJson(job).dump(2) + "\n");
    AppendLineToFile(events_path(job.job_id), SubmitEvent(job).dump());

    AutoDevSubmitResult result;
    result.success = true;
    result.job = std::move(job);
    result.job_dir = dir;
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
