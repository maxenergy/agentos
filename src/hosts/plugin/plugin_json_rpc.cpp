#include "hosts/plugin/plugin_json_rpc.hpp"

#include "hosts/plugin/plugin_host.hpp"
#include "hosts/plugin/plugin_json_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/spec_parsing.hpp"

#include <sstream>

namespace agentos {

std::optional<std::string> JsonRpcResultObject(const std::string& output_json) {
    const auto version = JsonStringField(output_json, "jsonrpc");
    if (!version.has_value() || *version != "2.0") {
        return std::nullopt;
    }
    if (!FindJsonValueStart(output_json, "id").has_value()) {
        return std::nullopt;
    }
    auto result_start = FindJsonValueStart(output_json, "result");
    if (!result_start.has_value() || *result_start >= output_json.size() || output_json[*result_start] != '{') {
        return std::nullopt;
    }
    return JsonObjectRawAt(output_json, *result_start);
}

std::string JsonRpcOutputError(const std::string& output_json) {
    if (!IsLikelyJsonObjectString(output_json)) {
        return "json-rpc-v0 plugin stdout must be a JSON object";
    }
    if (JsonStringField(output_json, "jsonrpc").value_or("") != "2.0") {
        return "json-rpc-v0 plugin stdout must include jsonrpc=\"2.0\"";
    }
    if (!FindJsonValueStart(output_json, "id").has_value()) {
        return "json-rpc-v0 plugin stdout must include id";
    }
    if (FindJsonValueStart(output_json, "error").has_value()) {
        return "json-rpc-v0 plugin returned an error response";
    }
    if (!JsonRpcResultObject(output_json).has_value()) {
        return "json-rpc-v0 plugin result must be a JSON object";
    }
    return {};
}

std::string JsonRpcRequestForPlugin(
    const PluginSpec& spec,
    const StringMap& arguments,
    const int request_id) {
    std::ostringstream params;
    params << '{';
    bool first = true;
    for (const auto& [key, value] : arguments) {
        if (!first) {
            params << ',';
        }
        first = false;
        params << QuoteJson(key) << ':' << QuoteJson(value);
    }
    params << '}';

    return MakeJsonObject({
        {"jsonrpc", QuoteJson("2.0")},
        {"id", NumberAsJson(request_id)},
        {"method", QuoteJson(spec.name)},
        {"params", params.str()},
    });
}

}  // namespace agentos
