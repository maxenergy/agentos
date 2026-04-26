#pragma once

#include "core/models.hpp"

#include <optional>
#include <string>

namespace agentos {

struct PluginSpec;

std::optional<std::string> JsonRpcResultObject(const std::string& output_json);
std::string JsonRpcOutputError(const std::string& output_json);
std::string JsonRpcRequestForPlugin(const PluginSpec& spec, const StringMap& arguments, int request_id);

}  // namespace agentos
