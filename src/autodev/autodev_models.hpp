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

struct AutoDevTurn {
    std::string turn_id;
    std::string job_id;
    std::string task_id;
    std::string adapter_kind;
    std::string adapter_name;
    std::string continuity_mode;
    std::string event_stream_mode;
    std::string session_id;
    std::optional<std::string> thread_id;
    std::optional<std::string> provider_turn_id;
    std::string status;
    std::string started_at;
    std::optional<std::string> completed_at;
    int duration_ms = 0;
    std::optional<std::filesystem::path> prompt_artifact;
    std::optional<std::filesystem::path> response_artifact;
    std::vector<std::string> changed_files;
    std::optional<std::string> summary;
    std::optional<std::string> error_code;
    std::optional<std::string> error_message;
};

struct AutoDevSnapshot {
    std::string snapshot_id;
    std::string job_id;
    std::string task_id;
    std::string spec_revision;
    std::string head_sha;
    std::vector<std::string> git_status;
    std::string captured_at;
    std::optional<std::filesystem::path> artifact_path;
};

struct AutoDevVerification {
    std::string verification_id;
    std::string job_id;
    std::string task_id;
    std::string spec_revision;
    std::string command;
    std::filesystem::path cwd;
    int exit_code = -1;
    bool passed = false;
    int duration_ms = 0;
    std::string started_at;
    std::string finished_at;
    std::optional<std::filesystem::path> output_log_path;
    std::optional<std::string> output_summary;
    std::optional<std::string> related_turn_id;
};

struct AutoDevDiffGuard {
    std::string diff_id;
    std::string job_id;
    std::string task_id;
    std::string spec_revision;
    bool passed = false;
    std::vector<std::string> changed_files;
    std::vector<std::string> allowed_files;
    std::vector<std::string> blocked_files;
    std::vector<std::string> blocked_file_violations;
    std::vector<std::string> outside_allowed_files;
    std::string checked_at;
};

struct AutoDevAcceptanceGate {
    std::string acceptance_id;
    std::string job_id;
    std::string task_id;
    std::string spec_revision;
    bool passed = false;
    std::optional<std::string> verification_id;
    std::optional<std::string> diff_id;
    std::vector<std::string> reasons;
    std::string checked_at;
};

struct AutoDevFinalReview {
    std::string final_review_id;
    std::string job_id;
    std::string spec_revision;
    bool passed = false;
    int tasks_total = 0;
    int tasks_passed = 0;
    std::vector<std::string> changed_files;
    std::vector<std::string> blocked_file_violations;
    std::vector<std::string> outside_allowed_files;
    std::vector<std::string> reasons;
    std::string checked_at;
};

nlohmann::json ToJson(const AutoDevSkillPackBinding& binding);
nlohmann::json ToJson(const AutoDevJob& job);
nlohmann::json ToJson(const AutoDevTask& task);
nlohmann::json ToJson(const AutoDevTurn& turn);
nlohmann::json ToJson(const AutoDevSnapshot& snapshot);
nlohmann::json ToJson(const AutoDevVerification& verification);
nlohmann::json ToJson(const AutoDevDiffGuard& diff_guard);
nlohmann::json ToJson(const AutoDevAcceptanceGate& acceptance);
nlohmann::json ToJson(const AutoDevFinalReview& final_review);
AutoDevJob AutoDevJobFromJson(const nlohmann::json& json);
AutoDevTask AutoDevTaskFromJson(const nlohmann::json& json);
AutoDevTurn AutoDevTurnFromJson(const nlohmann::json& json);
AutoDevSnapshot AutoDevSnapshotFromJson(const nlohmann::json& json);
AutoDevVerification AutoDevVerificationFromJson(const nlohmann::json& json);
AutoDevDiffGuard AutoDevDiffGuardFromJson(const nlohmann::json& json);
AutoDevAcceptanceGate AutoDevAcceptanceGateFromJson(const nlohmann::json& json);
AutoDevFinalReview AutoDevFinalReviewFromJson(const nlohmann::json& json);

}  // namespace agentos
