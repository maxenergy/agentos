#include "autodev/autodev_models.hpp"

#include <stdexcept>

namespace agentos {

namespace {

nlohmann::json OptionalString(const std::optional<std::string>& value) {
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json OptionalPath(const std::optional<std::filesystem::path>& value) {
    return value.has_value() ? nlohmann::json(value->string()) : nlohmann::json(nullptr);
}

std::optional<std::string> ReadOptionalString(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || json.at(key).is_null()) {
        return std::nullopt;
    }
    return json.at(key).get<std::string>();
}

std::optional<std::filesystem::path> ReadOptionalPath(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || json.at(key).is_null()) {
        return std::nullopt;
    }
    return std::filesystem::path(json.at(key).get<std::string>());
}

std::vector<std::string> ReadStringVector(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json.at(key).is_array()) {
        return {};
    }
    std::vector<std::string> values;
    for (const auto& value : json.at(key)) {
        if (value.is_string()) {
            values.push_back(value.get<std::string>());
        }
    }
    return values;
}

}  // namespace

nlohmann::json ToJson(const AutoDevSkillPackBinding& binding) {
    return nlohmann::json{
        {"name", OptionalString(binding.name)},
        {"source_type", OptionalString(binding.source_type)},
        {"source_uri", OptionalString(binding.source_uri)},
        {"local_path", OptionalPath(binding.local_path)},
        {"version", OptionalString(binding.version)},
        {"ref", OptionalString(binding.ref)},
        {"commit", OptionalString(binding.commit)},
        {"status", binding.status},
        {"loaded_at", OptionalString(binding.loaded_at)},
        {"manifest_hash", OptionalString(binding.manifest_hash)},
        {"error", OptionalString(binding.error)},
    };
}

nlohmann::json ToJson(const AutoDevJob& job) {
    return nlohmann::json{
        {"job_id", job.job_id},
        {"status", job.status},
        {"phase", job.phase},
        {"current_activity", job.current_activity},
        {"approval_gate", job.approval_gate},
        {"objective", job.objective},
        {"agentos_workspace", job.agentos_workspace.string()},
        {"target_repo_path", job.target_repo_path.string()},
        {"job_worktree_path", job.job_worktree_path.string()},
        {"isolation_mode", job.isolation_mode},
        {"isolation_status", job.isolation_status},
        {"allow_dirty_target", job.allow_dirty_target},
        {"created_from_head_sha", OptionalString(job.created_from_head_sha)},
        {"worktree_created_at", OptionalString(job.worktree_created_at)},
        {"worktree_cleanup_policy", job.worktree_cleanup_policy},
        {"next_action", job.next_action},
        {"blocker", OptionalString(job.blocker)},
        {"schema_version", OptionalString(job.schema_version)},
        {"spec_revision", OptionalString(job.spec_revision)},
        {"spec_hash", OptionalString(job.spec_hash)},
        {"skill_pack", ToJson(job.skill_pack)},
        {"created_at", job.created_at},
        {"updated_at", job.updated_at},
    };
}

AutoDevJob AutoDevJobFromJson(const nlohmann::json& json) {
    AutoDevJob job;
    job.job_id = json.at("job_id").get<std::string>();
    job.status = json.value("status", "submitted");
    job.phase = json.value("phase", "workspace_preparing");
    job.current_activity = json.value("current_activity", "none");
    job.approval_gate = json.value("approval_gate", "none");
    job.objective = json.value("objective", std::string{});
    job.agentos_workspace = json.at("agentos_workspace").get<std::string>();
    job.target_repo_path = json.at("target_repo_path").get<std::string>();
    job.job_worktree_path = json.at("job_worktree_path").get<std::string>();
    job.isolation_mode = json.value("isolation_mode", "git_worktree");
    job.isolation_status = json.value("isolation_status", "pending");
    job.allow_dirty_target = json.value("allow_dirty_target", false);
    job.created_from_head_sha = ReadOptionalString(json, "created_from_head_sha");
    job.worktree_created_at = ReadOptionalString(json, "worktree_created_at");
    job.worktree_cleanup_policy = json.value("worktree_cleanup_policy", "keep_until_done");
    job.next_action = json.value("next_action", "prepare_workspace");
    job.blocker = ReadOptionalString(json, "blocker");
    job.schema_version = ReadOptionalString(json, "schema_version");
    job.spec_revision = ReadOptionalString(json, "spec_revision");
    job.spec_hash = ReadOptionalString(json, "spec_hash");
    if (json.contains("skill_pack") && json.at("skill_pack").is_object()) {
        const auto& skill_pack = json.at("skill_pack");
        job.skill_pack.name = ReadOptionalString(skill_pack, "name");
        job.skill_pack.source_type = ReadOptionalString(skill_pack, "source_type");
        job.skill_pack.source_uri = ReadOptionalString(skill_pack, "source_uri");
        job.skill_pack.local_path = ReadOptionalPath(skill_pack, "local_path");
        job.skill_pack.version = ReadOptionalString(skill_pack, "version");
        job.skill_pack.ref = ReadOptionalString(skill_pack, "ref");
        job.skill_pack.commit = ReadOptionalString(skill_pack, "commit");
        job.skill_pack.status = skill_pack.value("status", "not_loaded");
        job.skill_pack.loaded_at = ReadOptionalString(skill_pack, "loaded_at");
        job.skill_pack.manifest_hash = ReadOptionalString(skill_pack, "manifest_hash");
        job.skill_pack.error = ReadOptionalString(skill_pack, "error");
    }
    job.created_at = json.at("created_at").get<std::string>();
    job.updated_at = json.at("updated_at").get<std::string>();
    return job;
}

nlohmann::json ToJson(const AutoDevTask& task) {
    return nlohmann::json{
        {"task_id", task.task_id},
        {"job_id", task.job_id},
        {"title", task.title},
        {"status", task.status},
        {"current_activity", task.current_activity},
        {"spec_revision", task.spec_revision},
        {"allowed_files", task.allowed_files},
        {"blocked_files", task.blocked_files},
        {"verify_command", OptionalString(task.verify_command)},
        {"acceptance_total", task.acceptance_total},
        {"acceptance_passed", task.acceptance_passed},
    };
}

AutoDevTask AutoDevTaskFromJson(const nlohmann::json& json) {
    AutoDevTask task;
    task.task_id = json.value("task_id", std::string{});
    task.job_id = json.value("job_id", std::string{});
    task.title = json.value("title", std::string{});
    task.status = json.value("status", "pending");
    task.current_activity = json.value("current_activity", "none");
    task.spec_revision = json.value("spec_revision", std::string{});
    task.allowed_files = ReadStringVector(json, "allowed_files");
    task.blocked_files = ReadStringVector(json, "blocked_files");
    task.verify_command = ReadOptionalString(json, "verify_command");
    task.acceptance_total = json.value("acceptance_total", 0);
    task.acceptance_passed = json.value("acceptance_passed", 0);
    return task;
}

nlohmann::json ToJson(const AutoDevTurn& turn) {
    return nlohmann::json{
        {"turn_id", turn.turn_id},
        {"job_id", turn.job_id},
        {"task_id", turn.task_id},
        {"adapter_kind", turn.adapter_kind},
        {"adapter_name", turn.adapter_name},
        {"continuity_mode", turn.continuity_mode},
        {"event_stream_mode", turn.event_stream_mode},
        {"session_id", turn.session_id},
        {"thread_id", OptionalString(turn.thread_id)},
        {"provider_turn_id", OptionalString(turn.provider_turn_id)},
        {"status", turn.status},
        {"started_at", turn.started_at},
        {"completed_at", OptionalString(turn.completed_at)},
        {"duration_ms", turn.duration_ms},
        {"prompt_artifact", OptionalPath(turn.prompt_artifact)},
        {"response_artifact", OptionalPath(turn.response_artifact)},
        {"changed_files", turn.changed_files},
        {"summary", OptionalString(turn.summary)},
        {"error_code", OptionalString(turn.error_code)},
        {"error_message", OptionalString(turn.error_message)},
    };
}

AutoDevTurn AutoDevTurnFromJson(const nlohmann::json& json) {
    AutoDevTurn turn;
    turn.turn_id = json.value("turn_id", std::string{});
    turn.job_id = json.value("job_id", std::string{});
    turn.task_id = json.value("task_id", std::string{});
    turn.adapter_kind = json.value("adapter_kind", std::string{});
    turn.adapter_name = json.value("adapter_name", std::string{});
    turn.continuity_mode = json.value("continuity_mode", std::string{});
    turn.event_stream_mode = json.value("event_stream_mode", std::string{});
    turn.session_id = json.value("session_id", std::string{});
    turn.thread_id = ReadOptionalString(json, "thread_id");
    turn.provider_turn_id = ReadOptionalString(json, "provider_turn_id");
    turn.status = json.value("status", std::string{});
    turn.started_at = json.value("started_at", std::string{});
    turn.completed_at = ReadOptionalString(json, "completed_at");
    turn.duration_ms = json.value("duration_ms", 0);
    turn.prompt_artifact = ReadOptionalPath(json, "prompt_artifact");
    turn.response_artifact = ReadOptionalPath(json, "response_artifact");
    turn.changed_files = ReadStringVector(json, "changed_files");
    turn.summary = ReadOptionalString(json, "summary");
    turn.error_code = ReadOptionalString(json, "error_code");
    turn.error_message = ReadOptionalString(json, "error_message");
    return turn;
}

nlohmann::json ToJson(const AutoDevSnapshot& snapshot) {
    return nlohmann::json{
        {"snapshot_id", snapshot.snapshot_id},
        {"job_id", snapshot.job_id},
        {"task_id", snapshot.task_id},
        {"spec_revision", snapshot.spec_revision},
        {"head_sha", snapshot.head_sha},
        {"git_status", snapshot.git_status},
        {"captured_at", snapshot.captured_at},
        {"artifact_path", OptionalPath(snapshot.artifact_path)},
    };
}

AutoDevSnapshot AutoDevSnapshotFromJson(const nlohmann::json& json) {
    AutoDevSnapshot snapshot;
    snapshot.snapshot_id = json.value("snapshot_id", std::string{});
    snapshot.job_id = json.value("job_id", std::string{});
    snapshot.task_id = json.value("task_id", std::string{});
    snapshot.spec_revision = json.value("spec_revision", std::string{});
    snapshot.head_sha = json.value("head_sha", std::string{});
    snapshot.git_status = ReadStringVector(json, "git_status");
    snapshot.captured_at = json.value("captured_at", std::string{});
    snapshot.artifact_path = ReadOptionalPath(json, "artifact_path");
    return snapshot;
}

nlohmann::json ToJson(const AutoDevRollback& rollback) {
    return nlohmann::json{
        {"rollback_id", rollback.rollback_id},
        {"job_id", rollback.job_id},
        {"task_id", rollback.task_id},
        {"mode", rollback.mode},
        {"status", rollback.status},
        {"reason", rollback.reason},
        {"target_files", rollback.target_files},
        {"destructive", rollback.destructive},
        {"executed", rollback.executed},
        {"recorded_at", rollback.recorded_at},
    };
}

AutoDevRollback AutoDevRollbackFromJson(const nlohmann::json& json) {
    AutoDevRollback rollback;
    rollback.rollback_id = json.value("rollback_id", std::string{});
    rollback.job_id = json.value("job_id", std::string{});
    rollback.task_id = json.value("task_id", std::string{});
    rollback.mode = json.value("mode", std::string{});
    rollback.status = json.value("status", std::string{});
    rollback.reason = json.value("reason", std::string{});
    rollback.target_files = ReadStringVector(json, "target_files");
    rollback.destructive = json.value("destructive", false);
    rollback.executed = json.value("executed", false);
    rollback.recorded_at = json.value("recorded_at", std::string{});
    return rollback;
}

nlohmann::json ToJson(const AutoDevVerification& verification) {
    return nlohmann::json{
        {"verification_id", verification.verification_id},
        {"job_id", verification.job_id},
        {"task_id", verification.task_id},
        {"spec_revision", verification.spec_revision},
        {"command", verification.command},
        {"cwd", verification.cwd.string()},
        {"exit_code", verification.exit_code},
        {"passed", verification.passed},
        {"duration_ms", verification.duration_ms},
        {"started_at", verification.started_at},
        {"finished_at", verification.finished_at},
        {"output_log_path", OptionalPath(verification.output_log_path)},
        {"output_summary", OptionalString(verification.output_summary)},
        {"related_turn_id", OptionalString(verification.related_turn_id)},
    };
}

AutoDevVerification AutoDevVerificationFromJson(const nlohmann::json& json) {
    AutoDevVerification verification;
    verification.verification_id = json.value("verification_id", std::string{});
    verification.job_id = json.value("job_id", std::string{});
    verification.task_id = json.value("task_id", std::string{});
    verification.spec_revision = json.value("spec_revision", std::string{});
    verification.command = json.value("command", std::string{});
    verification.cwd = json.value("cwd", std::string{});
    verification.exit_code = json.value("exit_code", -1);
    verification.passed = json.value("passed", false);
    verification.duration_ms = json.value("duration_ms", 0);
    verification.started_at = json.value("started_at", std::string{});
    verification.finished_at = json.value("finished_at", std::string{});
    verification.output_log_path = ReadOptionalPath(json, "output_log_path");
    verification.output_summary = ReadOptionalString(json, "output_summary");
    verification.related_turn_id = ReadOptionalString(json, "related_turn_id");
    return verification;
}

nlohmann::json ToJson(const AutoDevDiffGuard& diff_guard) {
    return nlohmann::json{
        {"diff_id", diff_guard.diff_id},
        {"job_id", diff_guard.job_id},
        {"task_id", diff_guard.task_id},
        {"spec_revision", diff_guard.spec_revision},
        {"passed", diff_guard.passed},
        {"changed_files", diff_guard.changed_files},
        {"allowed_files", diff_guard.allowed_files},
        {"blocked_files", diff_guard.blocked_files},
        {"blocked_file_violations", diff_guard.blocked_file_violations},
        {"outside_allowed_files", diff_guard.outside_allowed_files},
        {"checked_at", diff_guard.checked_at},
    };
}

AutoDevDiffGuard AutoDevDiffGuardFromJson(const nlohmann::json& json) {
    AutoDevDiffGuard diff_guard;
    diff_guard.diff_id = json.value("diff_id", std::string{});
    diff_guard.job_id = json.value("job_id", std::string{});
    diff_guard.task_id = json.value("task_id", std::string{});
    diff_guard.spec_revision = json.value("spec_revision", std::string{});
    diff_guard.passed = json.value("passed", false);
    diff_guard.changed_files = ReadStringVector(json, "changed_files");
    diff_guard.allowed_files = ReadStringVector(json, "allowed_files");
    diff_guard.blocked_files = ReadStringVector(json, "blocked_files");
    diff_guard.blocked_file_violations = ReadStringVector(json, "blocked_file_violations");
    diff_guard.outside_allowed_files = ReadStringVector(json, "outside_allowed_files");
    diff_guard.checked_at = json.value("checked_at", std::string{});
    return diff_guard;
}

nlohmann::json ToJson(const AutoDevAcceptanceGate& acceptance) {
    return nlohmann::json{
        {"acceptance_id", acceptance.acceptance_id},
        {"job_id", acceptance.job_id},
        {"task_id", acceptance.task_id},
        {"spec_revision", acceptance.spec_revision},
        {"passed", acceptance.passed},
        {"verification_id", OptionalString(acceptance.verification_id)},
        {"diff_id", OptionalString(acceptance.diff_id)},
        {"reasons", acceptance.reasons},
        {"checked_at", acceptance.checked_at},
    };
}

AutoDevAcceptanceGate AutoDevAcceptanceGateFromJson(const nlohmann::json& json) {
    AutoDevAcceptanceGate acceptance;
    acceptance.acceptance_id = json.value("acceptance_id", std::string{});
    acceptance.job_id = json.value("job_id", std::string{});
    acceptance.task_id = json.value("task_id", std::string{});
    acceptance.spec_revision = json.value("spec_revision", std::string{});
    acceptance.passed = json.value("passed", false);
    acceptance.verification_id = ReadOptionalString(json, "verification_id");
    acceptance.diff_id = ReadOptionalString(json, "diff_id");
    acceptance.reasons = ReadStringVector(json, "reasons");
    acceptance.checked_at = json.value("checked_at", std::string{});
    return acceptance;
}

nlohmann::json ToJson(const AutoDevFinalReview& final_review) {
    return nlohmann::json{
        {"final_review_id", final_review.final_review_id},
        {"job_id", final_review.job_id},
        {"spec_revision", final_review.spec_revision},
        {"passed", final_review.passed},
        {"tasks_total", final_review.tasks_total},
        {"tasks_passed", final_review.tasks_passed},
        {"changed_files", final_review.changed_files},
        {"blocked_file_violations", final_review.blocked_file_violations},
        {"outside_allowed_files", final_review.outside_allowed_files},
        {"reasons", final_review.reasons},
        {"checked_at", final_review.checked_at},
    };
}

AutoDevFinalReview AutoDevFinalReviewFromJson(const nlohmann::json& json) {
    AutoDevFinalReview final_review;
    final_review.final_review_id = json.value("final_review_id", std::string{});
    final_review.job_id = json.value("job_id", std::string{});
    final_review.spec_revision = json.value("spec_revision", std::string{});
    final_review.passed = json.value("passed", false);
    final_review.tasks_total = json.value("tasks_total", 0);
    final_review.tasks_passed = json.value("tasks_passed", 0);
    final_review.changed_files = ReadStringVector(json, "changed_files");
    final_review.blocked_file_violations = ReadStringVector(json, "blocked_file_violations");
    final_review.outside_allowed_files = ReadStringVector(json, "outside_allowed_files");
    final_review.reasons = ReadStringVector(json, "reasons");
    final_review.checked_at = json.value("checked_at", std::string{});
    return final_review;
}

}  // namespace agentos
