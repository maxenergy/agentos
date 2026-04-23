#include "skills/builtin/workflow_run_skill.hpp"

#include "utils/json_utils.hpp"

#include <sstream>

namespace agentos {

WorkflowRunSkill::WorkflowRunSkill(const SkillRegistry& skill_registry)
    : skill_registry_(skill_registry) {}

SkillManifest WorkflowRunSkill::manifest() const {
    return {
        .name = "workflow_run",
        .version = "0.1.0",
        .description = "Run a small built-in workflow through registered skills.",
        .capabilities = {"workflow", "skill_composition"},
        .input_schema_json = R"({"type":"object","required":["workflow","path"]})",
        .output_schema_json = R"({"type":"object","required":["workflow","steps"]})",
        .risk_level = "medium",
        .permissions = {"filesystem.read", "filesystem.write"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = 5000,
    };
}

SkillResult WorkflowRunSkill::execute(const SkillCall& call) {
    const auto workflow = call.get_arg("workflow").value_or("write_patch_read");
    if (workflow == "write_patch_read") {
        return RunWritePatchRead(call);
    }

    return {
        .success = false,
        .error_code = "WorkflowNotFound",
        .error_message = "unknown workflow: " + workflow,
    };
}

bool WorkflowRunSkill::healthy() const {
    return skill_registry_.find("file_write") && skill_registry_.find("file_patch") && skill_registry_.find("file_read");
}

SkillResult WorkflowRunSkill::RunWritePatchRead(const SkillCall& call) const {
    const auto file_write = skill_registry_.find("file_write");
    const auto file_patch = skill_registry_.find("file_patch");
    const auto file_read = skill_registry_.find("file_read");
    if (!file_write || !file_patch || !file_read) {
        return {false, "", "WorkflowDependencyMissing", "write_patch_read requires file_write, file_patch, and file_read", 0};
    }

    const auto path = call.get_arg("path");
    const auto content = call.get_arg("content");
    const auto find = call.get_arg("find");
    const auto replace = call.get_arg("replace");
    if (!path.has_value() || !content.has_value() || !find.has_value() || !replace.has_value()) {
        return {false, "", "InvalidArguments", "path, content, find, and replace are required", 0};
    }

    SkillCall write_call = call;
    write_call.call_id = call.call_id + ".file_write";
    write_call.skill_name = "file_write";

    const auto write_result = file_write->execute(write_call);
    if (!write_result.success) {
        return write_result;
    }

    SkillCall patch_call = call;
    patch_call.call_id = call.call_id + ".file_patch";
    patch_call.skill_name = "file_patch";

    const auto patch_result = file_patch->execute(patch_call);
    if (!patch_result.success) {
        return patch_result;
    }

    SkillCall read_call = call;
    read_call.call_id = call.call_id + ".file_read";
    read_call.skill_name = "file_read";

    const auto read_result = file_read->execute(read_call);
    if (!read_result.success) {
        return read_result;
    }

    const auto duration_ms = write_result.duration_ms + patch_result.duration_ms + read_result.duration_ms;
    return {
        .success = true,
        .json_output = MakeJsonObject({
            {"workflow", QuoteJson("write_patch_read")},
            {"steps", QuoteJson("file_write,file_patch,file_read")},
            {"final_output", QuoteJson(read_result.json_output)},
        }),
        .duration_ms = duration_ms,
    };
}

}  // namespace agentos

