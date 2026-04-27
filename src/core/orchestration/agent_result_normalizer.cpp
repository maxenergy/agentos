#include "core/orchestration/agent_result_normalizer.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

using Json = nlohmann::ordered_json;

std::optional<Json> ParseJsonObject(const std::string& value) {
    try {
        auto parsed = Json::parse(value);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

Json ObjectOrString(const std::string& value) {
    if (auto parsed = ParseJsonObject(value); parsed.has_value()) {
        return *parsed;
    }
    return value;
}

std::optional<std::string> StringField(const Json& object, const std::string_view key) {
    const auto found = object.find(std::string(key));
    if (found != object.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return std::nullopt;
}

Json OptionalString(const std::optional<std::string>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

Json ArtifactListJson(const std::vector<AgentArtifact>& artifacts) {
    auto output = Json::array();
    for (const auto& artifact : artifacts) {
        Json item;
        item["type"] = artifact.type;
        item["uri"] = artifact.uri;
        item["content"] = artifact.content;
        item["metadata"] = ObjectOrString(artifact.metadata_json);
        output.push_back(std::move(item));
    }
    return output;
}

Json ProviderMetadataJson(const std::string& agent_name, const std::optional<Json>& structured_output) {
    Json output;
    output["agent"] = structured_output.has_value()
        ? StringField(*structured_output, "agent").value_or(agent_name)
        : agent_name;
    output["provider"] = structured_output.has_value()
        ? OptionalString(StringField(*structured_output, "provider"))
        : nullptr;
    output["profile"] = structured_output.has_value()
        ? OptionalString(StringField(*structured_output, "profile"))
        : nullptr;
    output["auth_source"] = structured_output.has_value()
        ? OptionalString(StringField(*structured_output, "auth_source"))
        : nullptr;
    output["command"] = structured_output.has_value()
        ? OptionalString(StringField(*structured_output, "command"))
        : nullptr;

    if (structured_output.has_value()) {
        const auto exit_code = structured_output->find("exit_code");
        output["exit_code"] = exit_code != structured_output->end() ? *exit_code : Json(nullptr);
    } else {
        output["exit_code"] = nullptr;
    }
    return output;
}

}  // namespace

std::string BuildNormalizedAgentResultJson(const NormalizedAgentResultInput& input) {
    const auto structured_output = ParseJsonObject(input.structured_output_json);
    const auto model = structured_output.has_value()
        ? StringField(*structured_output, "model")
        : std::optional<std::string>{};
    const auto content = structured_output.has_value()
        ? StringField(*structured_output, "content").value_or(input.summary)
        : input.summary;

    Json metrics;
    metrics["duration_ms"] = input.duration_ms;
    metrics["estimated_cost"] = input.estimated_cost;

    Json output;
    output["schema_version"] = "agent_result.v1";
    output["agent"] = input.agent_name;
    output["success"] = input.success;
    output["summary"] = input.summary;
    output["content"] = content;
    output["model"] = OptionalString(model);
    output["artifacts"] = ArtifactListJson(input.artifacts);
    output["metrics"] = std::move(metrics);
    output["tool_calls"] = Json::array();
    output["provider_metadata"] = ProviderMetadataJson(input.agent_name, structured_output);
    output["error_code"] = input.error_code.empty() ? Json(nullptr) : Json(input.error_code);
    output["error_message"] = input.error_message.empty() ? Json(nullptr) : Json(input.error_message);
    output["raw_output"] = structured_output.has_value() ? *structured_output : Json(input.structured_output_json);
    return output.dump();
}

}  // namespace agentos
