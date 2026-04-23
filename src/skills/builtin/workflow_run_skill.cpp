#include "skills/builtin/workflow_run_skill.hpp"

#include "utils/json_utils.hpp"

#include <algorithm>
#include <sstream>

namespace agentos {

WorkflowRunSkill::WorkflowRunSkill(const SkillRegistry& skill_registry, const WorkflowStore* workflow_store)
    : skill_registry_(skill_registry),
      workflow_store_(workflow_store) {}

SkillManifest WorkflowRunSkill::manifest() const {
    return {
        .name = "workflow_run",
        .version = "0.1.0",
        .description = "Run a built-in or promoted workflow through registered skills.",
        .capabilities = {"workflow", "skill_composition"},
        .input_schema_json = R"({"type":"object","required":["workflow"]})",
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

    if (workflow_store_ != nullptr) {
        const auto stored_workflow = workflow_store_->find(workflow);
        if (stored_workflow.has_value()) {
            if (!stored_workflow->enabled) {
                return {
                    .success = false,
                    .error_code = "WorkflowDisabled",
                    .error_message = "workflow is disabled: " + workflow,
                };
            }
            return RunStoredWorkflow(*stored_workflow, call);
        }
    }

    return {
        .success = false,
        .error_code = "WorkflowNotFound",
        .error_message = "unknown workflow: " + workflow,
    };
}

bool WorkflowRunSkill::healthy() const {
    return (skill_registry_.find("file_write") && skill_registry_.find("file_patch") && skill_registry_.find("file_read")) ||
           (workflow_store_ != nullptr && !workflow_store_->list().empty());
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

SkillResult WorkflowRunSkill::RunStoredWorkflow(const WorkflowDefinition& workflow, const SkillCall& call) const {
    if (workflow.ordered_steps.empty()) {
        return {
            .success = false,
            .error_code = "InvalidWorkflowDefinition",
            .error_message = "stored workflow has no steps: " + workflow.name,
        };
    }

    std::vector<std::string> completed_steps;
    completed_steps.reserve(workflow.ordered_steps.size());
    std::string final_output;
    int duration_ms = 0;

    for (const auto& step_name : workflow.ordered_steps) {
        if (step_name == "workflow_run") {
            return {
                .success = false,
                .error_code = "RecursiveWorkflowStep",
                .error_message = "stored workflow cannot call workflow_run recursively",
                .duration_ms = duration_ms,
            };
        }

        const auto step = skill_registry_.find(step_name);
        if (!step) {
            return {
                .success = false,
                .error_code = "WorkflowDependencyMissing",
                .error_message = "stored workflow requires missing skill: " + step_name,
                .duration_ms = duration_ms,
            };
        }

        const auto manifest = step->manifest();
        if (!StoredStepIsInPolicyScope(manifest)) {
            return {
                .success = false,
                .error_code = "WorkflowPermissionOutOfScope",
                .error_message = "stored workflow step is outside workflow_run policy scope: " + step_name,
                .duration_ms = duration_ms,
            };
        }

        SkillCall step_call = call;
        step_call.call_id = call.call_id + "." + step_name;
        step_call.skill_name = step_name;

        const auto step_result = step->execute(step_call);
        duration_ms += step_result.duration_ms;
        if (!step_result.success) {
            return {
                .success = false,
                .error_code = step_result.error_code.empty() ? "WorkflowStepFailed" : step_result.error_code,
                .error_message = "stored workflow step failed: " + step_name + " " + step_result.error_message,
                .duration_ms = duration_ms,
            };
        }

        completed_steps.push_back(step_name);
        final_output = step_result.json_output;
    }

    std::ostringstream steps;
    for (std::size_t index = 0; index < completed_steps.size(); ++index) {
        if (index != 0) {
            steps << ',';
        }
        steps << completed_steps[index];
    }

    return {
        .success = true,
        .json_output = MakeJsonObject({
            {"workflow", QuoteJson(workflow.name)},
            {"source", QuoteJson(workflow.source)},
            {"steps", QuoteJson(steps.str())},
            {"final_output", QuoteJson(final_output)},
        }),
        .duration_ms = duration_ms,
    };
}

bool WorkflowRunSkill::StoredStepIsInPolicyScope(const SkillManifest& manifest) const {
    if (manifest.risk_level != "low" && manifest.risk_level != "medium") {
        return false;
    }

    const auto workflow_manifest = this->manifest();
    for (const auto& permission : manifest.permissions) {
        if (std::find(workflow_manifest.permissions.begin(), workflow_manifest.permissions.end(), permission) ==
            workflow_manifest.permissions.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace agentos
