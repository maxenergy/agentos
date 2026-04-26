#include "core/orchestration/agent_result_normalizer.hpp"

#include "utils/json_utils.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentos {

namespace {

bool IsJsonObjectLike(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    return first != std::string::npos && last != std::string::npos && value[first] == '{' && value[last] == '}';
}

std::optional<std::string> JsonStringField(const std::string& json, const std::string_view key) {
    const auto marker = "\"" + std::string(key) + "\"";
    auto cursor = json.find(marker);
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    cursor = json.find(':', cursor + marker.size());
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    cursor = json.find('"', cursor + 1);
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    ++cursor;

    std::string value;
    bool escaping = false;
    for (; cursor < json.size(); ++cursor) {
        const auto ch = json[cursor];
        if (escaping) {
            switch (ch) {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::string> JsonScalarField(const std::string& json, const std::string_view key) {
    const auto marker = "\"" + std::string(key) + "\"";
    auto cursor = json.find(marker);
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    cursor = json.find(':', cursor + marker.size());
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    ++cursor;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\r' || json[cursor] == '\n')) {
        ++cursor;
    }
    const auto start = cursor;
    while (cursor < json.size() && json[cursor] != ',' && json[cursor] != '}') {
        ++cursor;
    }
    if (cursor <= start) {
        return std::nullopt;
    }
    return json.substr(start, cursor - start);
}

std::string OptionalStringJson(const std::optional<std::string>& value) {
    return value.has_value() ? QuoteJson(*value) : "null";
}

std::string ArtifactListJson(const std::vector<AgentArtifact>& artifacts) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const auto& artifact = artifacts[index];
        output << MakeJsonObject({
            {"type", QuoteJson(artifact.type)},
            {"uri", QuoteJson(artifact.uri)},
            {"content", QuoteJson(artifact.content)},
            {"metadata", IsJsonObjectLike(artifact.metadata_json) ? artifact.metadata_json : QuoteJson(artifact.metadata_json)},
        });
    }
    output << ']';
    return output.str();
}

std::string ProviderMetadataJson(const std::string& agent_name, const std::string& structured_output_json) {
    return MakeJsonObject({
        {"agent", QuoteJson(JsonStringField(structured_output_json, "agent").value_or(agent_name))},
        {"provider", OptionalStringJson(JsonStringField(structured_output_json, "provider"))},
        {"profile", OptionalStringJson(JsonStringField(structured_output_json, "profile"))},
        {"auth_source", OptionalStringJson(JsonStringField(structured_output_json, "auth_source"))},
        {"command", OptionalStringJson(JsonStringField(structured_output_json, "command"))},
        {"exit_code", JsonScalarField(structured_output_json, "exit_code").value_or("null")},
    });
}

}  // namespace

std::string BuildNormalizedAgentResultJson(const NormalizedAgentResultInput& input) {
    const auto model = JsonStringField(input.structured_output_json, "model");
    const auto content = JsonStringField(input.structured_output_json, "content").value_or(input.summary);
    const auto metrics = MakeJsonObject({
        {"duration_ms", NumberAsJson(input.duration_ms)},
        {"estimated_cost", NumberAsJson(input.estimated_cost)},
    });

    return MakeJsonObject({
        {"schema_version", QuoteJson("agent_result.v1")},
        {"agent", QuoteJson(input.agent_name)},
        {"success", BoolAsJson(input.success)},
        {"summary", QuoteJson(input.summary)},
        {"content", QuoteJson(content)},
        {"model", OptionalStringJson(model)},
        {"artifacts", ArtifactListJson(input.artifacts)},
        {"metrics", metrics},
        {"tool_calls", "[]"},
        {"provider_metadata", ProviderMetadataJson(input.agent_name, input.structured_output_json)},
        {"error_code", input.error_code.empty() ? "null" : QuoteJson(input.error_code)},
        {"error_message", input.error_message.empty() ? "null" : QuoteJson(input.error_message)},
        {"raw_output", IsJsonObjectLike(input.structured_output_json) ? input.structured_output_json : QuoteJson(input.structured_output_json)},
    });
}

}  // namespace agentos
