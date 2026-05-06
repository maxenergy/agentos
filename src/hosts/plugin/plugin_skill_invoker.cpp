#include "hosts/plugin/plugin_host.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>

namespace agentos {

namespace {

std::optional<nlohmann::ordered_json> ParseObject(const std::string& json_text) {
    try {
        auto parsed = nlohmann::ordered_json::parse(json_text);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

nlohmann::ordered_json PluginOutputValue(const PluginSpec& spec, const PluginRunResult& run_result) {
    (void)spec;
    if (!run_result.success) {
        return nullptr;
    }

    if (auto parsed = ParseObject(run_result.structured_output_json); parsed.has_value()) {
        return *parsed;
    }
    return nullptr;
}

std::string PluginSkillOutputJson(const PluginSpec& spec, const PluginRunResult& run_result) {
    nlohmann::ordered_json output;
    output["plugin"] = spec.name;
    output["manifest_version"] = spec.manifest_version;
    output["protocol"] = spec.protocol;
    output["lifecycle_mode"] = spec.lifecycle_mode;
    output["lifecycle_event"] = run_result.lifecycle_event.empty() ? "oneshot" : run_result.lifecycle_event;
    output["plugin_output"] = PluginOutputValue(spec, run_result);
    return output.dump();
}

}  // namespace

PluginSkillInvoker::PluginSkillInvoker(PluginSpec spec, const PluginHost& plugin_host)
    : spec_(std::move(spec)), plugin_host_(plugin_host) {}

SkillManifest PluginSkillInvoker::manifest() const {
    return {
        .name = spec_.name,
        .version = spec_.manifest_version,
        .description = spec_.description,
        .capabilities = {"plugin", spec_.protocol, spec_.lifecycle_mode},
        .input_schema_json = spec_.input_schema_json,
        .output_schema_json = spec_.output_schema_json,
        .risk_level = spec_.risk_level,
        .permissions = spec_.permissions,
        .idempotent = spec_.idempotent,
        .timeout_ms = spec_.timeout_ms,
    };
}

SkillResult PluginSkillInvoker::execute(const SkillCall& call) {
    const auto run_result = plugin_host_.run(PluginRunRequest{
        .spec = spec_,
        .arguments = call.arguments,
        .workspace_path = std::filesystem::path(call.workspace_id),
    });

    SkillResult result{
        .success = run_result.success,
        .json_output = PluginSkillOutputJson(spec_, run_result),
        .error_code = run_result.error_code,
        .error_message = run_result.error_message,
        .duration_ms = run_result.duration_ms,
    };
    return result;
}

bool PluginSkillInvoker::healthy() const {
    return CheckPluginHealth(spec_).healthy;
}

}  // namespace agentos
