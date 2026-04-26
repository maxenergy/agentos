#pragma once

#include <string>

namespace agentos {

struct PluginSpec;

std::string PluginOutputSchemaError(const PluginSpec& spec, const std::string& output_json);

}  // namespace agentos
