#include "hosts/plugin/plugin_host.hpp"

#include "utils/json_utils.hpp"

#include <filesystem>
#include <sstream>

namespace agentos {

namespace {

std::string PluginSkillOutputJson(const PluginSpec& spec, const PluginRunResult& run_result) {
    auto plugin_output = run_result.stdout_text;
    if (run_result.success && spec.protocol == "json-rpc-v0") {
        const auto result_key = std::string("\"result\"");
        const auto key_pos = plugin_output.find(result_key);
        if (key_pos != std::string::npos) {
            const auto colon_pos = plugin_output.find(':', key_pos + result_key.size());
            const auto object_start = colon_pos == std::string::npos
                ? std::string::npos
                : plugin_output.find('{', colon_pos + 1);
            if (object_start != std::string::npos) {
                int depth = 0;
                bool in_string = false;
                bool escaped = false;
                for (std::size_t pos = object_start; pos < plugin_output.size(); ++pos) {
                    const char ch = plugin_output[pos];
                    if (escaped) {
                        escaped = false;
                        continue;
                    }
                    if (ch == '\\') {
                        escaped = true;
                        continue;
                    }
                    if (ch == '"') {
                        in_string = !in_string;
                        continue;
                    }
                    if (in_string) {
                        continue;
                    }
                    if (ch == '{') {
                        ++depth;
                    } else if (ch == '}') {
                        --depth;
                        if (depth == 0) {
                            plugin_output = plugin_output.substr(object_start, pos - object_start + 1);
                            break;
                        }
                    }
                }
            }
        }
    }

    std::ostringstream output;
    output
        << "{"
        << R"("plugin":")" << EscapeJson(spec.name) << R"(",)"
        << R"("manifest_version":")" << EscapeJson(spec.manifest_version) << R"(",)"
        << R"("protocol":")" << EscapeJson(spec.protocol) << R"(",)"
        << R"("lifecycle_mode":")" << EscapeJson(spec.lifecycle_mode) << R"(",)"
        << R"("lifecycle_event":")" << EscapeJson(run_result.lifecycle_event.empty() ? "oneshot" : run_result.lifecycle_event) << R"(",)"
        << R"("plugin_output":)";
    if (run_result.success) {
        output << plugin_output;
    } else {
        output << "null";
    }
    output << "}";
    return output.str();
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
        .workspace_path = std::filesystem::current_path(),
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
