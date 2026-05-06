#pragma once

#include "autodev/autodev_execution_adapter.hpp"
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

struct AutoDevApproveSpecResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    std::string spec_revision;
    std::string spec_hash;
    std::filesystem::path status_path;
    std::filesystem::path tasks_path;
};

struct AutoDevVerifyTaskResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevTask task;
    AutoDevVerification verification;
    std::filesystem::path verification_path;
    std::filesystem::path verify_report_path;
};

struct AutoDevSnapshotResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevTask task;
    AutoDevSnapshot snapshot;
    std::filesystem::path snapshots_path;
    std::filesystem::path snapshot_artifact_path;
};

struct AutoDevRollbackResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevTask task;
    AutoDevRollback rollback;
    std::filesystem::path rollbacks_path;
};

struct AutoDevDiffGuardResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevTask task;
    AutoDevDiffGuard diff_guard;
    std::filesystem::path diffs_path;
};

struct AutoDevAcceptanceGateResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevTask task;
    AutoDevAcceptanceGate acceptance;
    std::filesystem::path acceptance_path;
};

struct AutoDevFinalReviewResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevFinalReview final_review;
    std::filesystem::path final_review_path;
    std::filesystem::path final_review_report_path;
};

struct AutoDevCompleteJobResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    AutoDevFinalReview final_review;
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
    [[nodiscard]] std::filesystem::path tasks_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path turns_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path snapshots_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path snapshots_dir(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path rollbacks_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path repairs_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path verification_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path diffs_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path acceptance_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path final_review_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path logs_dir(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path artifacts_dir(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path spec_revisions_dir(const std::string& job_id) const;

    AutoDevSubmitResult submit(const AutoDevSubmitRequest& request);
    AutoDevPrepareWorkspaceResult prepare_workspace(const std::string& job_id);
    AutoDevLoadSkillPackResult load_skill_pack(
        const std::string& job_id,
        const std::optional<std::filesystem::path>& override_path = std::nullopt);
    AutoDevGenerateGoalDocsResult generate_goal_docs(const std::string& job_id);
    AutoDevValidateSpecResult validate_spec(const std::string& job_id);
    AutoDevApproveSpecResult approve_spec(
        const std::string& job_id,
        const std::string& spec_hash,
        const std::optional<std::string>& spec_revision = std::nullopt);
    void record_execution_blocked(
        const AutoDevJob& job,
        const AutoDevTask& task,
        const AutoDevExecutionAdapterProfile& adapter_profile,
        const std::string& reason);
    AutoDevSnapshotResult record_task_snapshot(const std::string& job_id, const std::string& task_id);
    AutoDevRollbackResult rollback_soft(const std::string& job_id, const std::string& task_id);
    AutoDevRollbackResult rollback_hard(
        const std::string& job_id,
        const std::string& task_id,
        const std::optional<std::string>& approval);
    AutoDevVerifyTaskResult verify_task(
        const std::string& job_id,
        const std::string& task_id,
        const std::optional<std::string>& related_turn_id = std::nullopt);
    AutoDevDiffGuardResult diff_guard(const std::string& job_id, const std::string& task_id);
    AutoDevAcceptanceGateResult acceptance_gate(const std::string& job_id, const std::string& task_id);
    AutoDevFinalReviewResult final_review(const std::string& job_id);
    AutoDevCompleteJobResult complete_job(const std::string& job_id);
    std::optional<AutoDevJob> load_job(const std::string& job_id, std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevTask>> load_tasks(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevTurn>> load_turns(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevSnapshot>> load_snapshots(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevRollback>> load_rollbacks(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevRepairNeeded>> load_repairs(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevVerification>> load_verifications(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevDiffGuard>> load_diffs(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevAcceptanceGate>> load_acceptances(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<AutoDevFinalReview>> load_final_reviews(
        const std::string& job_id,
        std::string* error_message = nullptr) const;
    std::optional<std::vector<std::string>> load_event_lines(
        const std::string& job_id,
        std::string* error_message = nullptr) const;

private:
    std::filesystem::path agentos_workspace_;
    void save_job(const AutoDevJob& job) const;
    void append_event(const std::string& job_id, const nlohmann::json& event) const;
    AutoDevRepairNeeded record_repair_needed(
        const AutoDevJob& job,
        const AutoDevTask& task,
        const std::string& source_type,
        const std::string& source_id,
        const std::vector<std::string>& reasons);
};

}  // namespace agentos
