#pragma once

#include "autodev/autodev_models.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace agentos {

struct AutoDevSubmitRequest {
    std::filesystem::path agentos_workspace;
    std::filesystem::path target_repo_path;
    std::string objective;
    std::optional<std::filesystem::path> skill_pack_path;
    std::string isolation_mode = "git_worktree";
    bool allow_dirty_target = false;
};

struct AutoDevSubmitResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    std::filesystem::path job_dir;
};

struct AutoDevPrepareWorkspaceResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
};

struct AutoDevLoadSkillPackResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    std::filesystem::path snapshot_path;
};

struct AutoDevGenerateGoalDocsResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    std::filesystem::path goal_dir;
    std::vector<std::filesystem::path> written_files;
};

struct AutoDevValidateSpecResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    std::string spec_revision;
    std::string spec_hash;
    std::filesystem::path normalized_path;
    std::filesystem::path hash_path;
    std::filesystem::path status_path;
};

class AutoDevStateStore {
public:
    explicit AutoDevStateStore(std::filesystem::path agentos_workspace);

    [[nodiscard]] const std::filesystem::path& agentos_workspace() const;
    [[nodiscard]] std::filesystem::path root_dir() const;
    [[nodiscard]] std::filesystem::path jobs_dir() const;
    [[nodiscard]] std::filesystem::path job_dir(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path job_json_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path events_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path artifacts_dir(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path spec_revisions_dir(const std::string& job_id) const;

    AutoDevSubmitResult submit(const AutoDevSubmitRequest& request);
    AutoDevPrepareWorkspaceResult prepare_workspace(const std::string& job_id);
    AutoDevLoadSkillPackResult load_skill_pack(
        const std::string& job_id,
        const std::optional<std::filesystem::path>& override_path = std::nullopt);
    AutoDevGenerateGoalDocsResult generate_goal_docs(const std::string& job_id);
    AutoDevValidateSpecResult validate_spec(const std::string& job_id);
    std::optional<AutoDevJob> load_job(const std::string& job_id, std::string* error_message = nullptr) const;

private:
    std::filesystem::path agentos_workspace_;
    void save_job(const AutoDevJob& job) const;
    void append_event(const std::string& job_id, const nlohmann::json& event) const;
};

}  // namespace agentos
