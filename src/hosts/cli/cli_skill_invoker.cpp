#include "hosts/cli/cli_skill_invoker.hpp"

#include "utils/command_utils.hpp"

#include <utility>

#include <nlohmann/json.hpp>

namespace agentos {

CliSkillInvoker::CliSkillInvoker(CliSpec spec, const CliHost& cli_host)
    : spec_(std::move(spec)),
      cli_host_(cli_host) {}

SkillManifest CliSkillInvoker::manifest() const {
    return {
        .name = spec_.name,
        .version = "0.1.0",
        .description = spec_.description,
        .capabilities = {"cli", spec_.parse_mode},
        .input_schema_json = spec_.input_schema_json,
        .output_schema_json = spec_.output_schema_json,
        .risk_level = spec_.risk_level,
        .permissions = spec_.permissions,
        .supports_streaming = false,
        .idempotent = true,
        .timeout_ms = spec_.timeout_ms,
    };
}

SkillResult CliSkillInvoker::execute(const SkillCall& call) {
    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec_,
        .arguments = call.arguments,
        .workspace_path = call.workspace_id,
    });

    nlohmann::json output_json;
    output_json["command"] = result.command_display;
    output_json["exit_code"] = result.exit_code;
    output_json["timed_out"] = result.timed_out;
    output_json["stdout"] = result.stdout_text;
    output_json["stderr"] = result.stderr_text;
    return {
        .success = result.success,
        .json_output = output_json.dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace),
        .error_code = result.error_code,
        .error_message = result.error_message,
        .duration_ms = result.duration_ms,
    };
}

bool CliSkillInvoker::healthy() const {
    return CommandExists(spec_.binary);
}

CliSpec MakeRgSearchSpec() {
    return {
        .name = "rg_search",
        .description = "Search files in the active workspace through ripgrep JSON output.",
        .binary = "rg",
        .args_template = {"--json", "{{pattern}}", "{{path}}"},
        .required_args = {"pattern", "path"},
        .input_schema_json = R"({"type":"object","required":["pattern","path"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "json_lines",
        .risk_level = "low",
        .permissions = {"filesystem.read", "process.spawn"},
        .timeout_ms = 3000,
    };
}

CliSpec MakeGitStatusSpec() {
    return {
        .name = "git_status",
        .description = "Run git status --short --branch inside the active workspace.",
        .binary = "git",
        .args_template = {"status", "--short", "--branch"},
        .input_schema_json = R"({"type":"object","properties":{"cwd":{"type":"string"}}})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"filesystem.read", "process.spawn"},
        .timeout_ms = 3000,
    };
}

CliSpec MakeGitDiffSpec() {
    return {
        .name = "git_diff",
        .description = "Run git diff --stat or full git diff inside the active workspace.",
        .binary = "git",
        .args_template = {"diff", "{{mode}}"},
        .input_schema_json = R"({"type":"object","properties":{"mode":{"type":"string"},"cwd":{"type":"string"}}})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "low",
        .permissions = {"filesystem.read", "process.spawn"},
        .timeout_ms = 5000,
    };
}

CliSpec MakeCurlFetchSpec() {
    return {
        .name = "curl_fetch",
        .description = "Fetch a URL through curl with timeout and output capture.",
        .binary = "curl",
        .args_template = {"-L", "--silent", "--show-error", "--max-time", "10", "{{url}}"},
        .required_args = {"url"},
        .input_schema_json = R"({"type":"object","required":["url"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = 12000,
    };
}

CliSpec MakeJqTransformSpec() {
    return {
        .name = "jq_transform",
        .description = "Transform a workspace JSON file through jq compact output.",
        .binary = "jq",
        .args_template = {"-c", "{{filter}}", "{{path}}"},
        .required_args = {"filter", "path"},
        .input_schema_json = R"({"type":"object","required":["filter","path"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "json",
        .risk_level = "low",
        .permissions = {"filesystem.read", "process.spawn"},
        .timeout_ms = 3000,
    };
}

}  // namespace agentos
