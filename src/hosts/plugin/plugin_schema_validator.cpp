#include "hosts/plugin/plugin_schema_validator.hpp"

#include "core/schema/schema_validator.hpp"
#include "hosts/plugin/plugin_host.hpp"

namespace agentos {

std::string PluginOutputSchemaError(const PluginSpec& spec, const std::string& output_json) {
    return JsonObjectSchemaValidationError(spec.output_schema_json, output_json, "plugin output");
}

}  // namespace agentos
