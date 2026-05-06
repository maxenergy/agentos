#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

struct AutoDevSkillPackBinding {
    std::optional<std::string> name;
    std::optional<std::string> source_type;
    std::optional<std::string> source_uri;
    std::optional<std::filesystem::path> local_path;
    std::optional<std::string> version;
    std::optional<std::string> ref;
    std::optional<std::string> commit;
    std::string status = "not_loaded";
    std::optional<std::string> loaded_at;
    std::optional<std::string> manifest_hash;
    std::optional<std::string> error;
};

struct AutoDevJob {
    std::string job_id;
    std::string status = "submitted";
    std::string phase = "workspace_preparing";
    std::string current_activity = "none";
    std::string approval_gate = "none";
    std::string objective;

    std::filesystem::path agentos_workspace;
    std::filesystem::path target_repo_path;
    std::filesystem::path job_worktree_path;

    std::string isolation_mode = "git_worktree";
    std::string isolation_status = "pending";
    bool allow_dirty_target = false;
    std::optional<std::string> created_from_head_sha;
    std::optional<std::string> worktree_created_at;
    std::string worktree_cleanup_policy = "keep_until_done";

    std::string next_action = "prepare_workspace";
    std::optional<std::string> blocker;
    std::optional<std::string> schema_version;
    std::optional<std::string> spec_revision;
    std::optional<std::string> spec_hash;
    AutoDevSkillPackBinding skill_pack;

    std::string created_at;
    std::string updated_at;
};

struct AutoDevTask {
    std::string task_id;
    std::string job_id;
    std::string title;
    std::string status = "pending";
    std::string current_activity = "none";
    std::string spec_revision;
    std::vector<std::string> allowed_files;
    std::vector<std::string> blocked_files;
    std::optional<std::string> verify_command;
    int acceptance_total = 0;
    int acceptance_passed = 0;
};

nlohmann::json ToJson(const AutoDevSkillPackBinding& binding);
nlohmann::json ToJson(const AutoDevJob& job);
nlohmann::json ToJson(const AutoDevTask& task);
AutoDevJob AutoDevJobFromJson(const nlohmann::json& json);
AutoDevTask AutoDevTaskFromJson(const nlohmann::json& json);

}  // namespace agentos
