#include "hosts/plugin/plugin_json_rpc.hpp"

#include "hosts/plugin/plugin_host.hpp"

#include <nlohmann/json.hpp>

namespace agentos {
namespace {

std::optional<nlohmann::ordered_json> ParseJsonObject(const std::string& output_json) {
    try {
        auto parsed = nlohmann::ordered_json::parse(output_json);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

std::string JsonRpcRequest(
    const std::string& method,
    const StringMap& arguments,
    const int request_id) {
    auto params = nlohmann::ordered_json::object();
    for (const auto& [key, value] : arguments) {
        params[key] = value;
    }

    nlohmann::ordered_json request;
    request["jsonrpc"] = "2.0";
    request["id"] = request_id;
    request["method"] = method;
    request["params"] = std::move(params);
    return request.dump();
}

}  // namespace

std::optional<std::string> JsonRpcResultObject(const std::string& output_json) {
    auto response = ParseJsonObject(output_json);
    if (!response.has_value()) {
        return std::nullopt;
    }

    const auto version = response->find("jsonrpc");
    if (version == response->end() || !version->is_string() || *version != "2.0") {
        return std::nullopt;
    }
    if (!response->contains("id")) {
        return std::nullopt;
    }
    const auto result = response->find("result");
    if (result == response->end() || !result->is_object()) {
        return std::nullopt;
    }
    return result->dump();
}

std::string JsonRpcOutputError(const std::string& output_json) {
    auto response = ParseJsonObject(output_json);
    if (!response.has_value()) {
        return "json-rpc-v0 plugin stdout must be a JSON object";
    }

    const auto version = response->find("jsonrpc");
    if (version == response->end() || !version->is_string() || *version != "2.0") {
        return "json-rpc-v0 plugin stdout must include jsonrpc=\"2.0\"";
    }
    if (!response->contains("id")) {
        return "json-rpc-v0 plugin stdout must include id";
    }
    if (response->contains("error")) {
        return "json-rpc-v0 plugin returned an error response";
    }
    const auto result = response->find("result");
    if (result == response->end() || !result->is_object()) {
        return "json-rpc-v0 plugin result must be a JSON object";
    }
    return {};
}

std::string JsonRpcRequestForPlugin(
    const PluginSpec& spec,
    const StringMap& arguments,
    const int request_id) {
    return JsonRpcRequest(spec.name, arguments, request_id);
}

std::string JsonRpcHealthRequest(const int request_id) {
    return JsonRpcRequest("$/healthz", {}, request_id);
}

}  // namespace agentos
