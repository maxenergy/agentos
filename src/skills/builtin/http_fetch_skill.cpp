#include "skills/builtin/http_fetch_skill.hpp"

#include "utils/json_utils.hpp"

namespace agentos {

HttpFetchSkill::HttpFetchSkill(const CliHost& cli_host)
    : cli_host_(cli_host) {}

SkillManifest HttpFetchSkill::manifest() const {
    return {
        .name = "http_fetch",
        .version = "0.1.0",
        .description = "Fetch HTTP content through the controlled CLI host.",
        .capabilities = {"network", "http", "fetch"},
        .input_schema_json = R"({"type":"object","required":["url"]})",
        .output_schema_json = R"({"type":"object","required":["url","body"]})",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .supports_streaming = false,
        .idempotent = true,
        .timeout_ms = 12000,
    };
}

SkillResult HttpFetchSkill::execute(const SkillCall& call) {
    const auto maybe_url = call.get_arg("url");
    if (!maybe_url.has_value()) {
        return {false, "", "InvalidArguments", "url is required", 0};
    }

    const CliSpec spec{
        .name = "http_fetch",
        .description = "Fetch HTTP content through curl.",
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

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = call.arguments,
        .workspace_path = call.workspace_id,
    });

    return {
        .success = result.success,
        .json_output = MakeJsonObject({
            {"url", QuoteJson(*maybe_url)},
            {"body", QuoteJson(result.stdout_text)},
            {"stderr", QuoteJson(result.stderr_text)},
            {"exit_code", NumberAsJson(result.exit_code)},
            {"timed_out", BoolAsJson(result.timed_out)},
        }),
        .error_code = result.error_code,
        .error_message = result.error_message,
        .duration_ms = result.duration_ms,
    };
}

bool HttpFetchSkill::healthy() const {
    return true;
}

}  // namespace agentos

