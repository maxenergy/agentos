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

}  // namespace agentos
