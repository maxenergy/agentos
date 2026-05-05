#include "hosts/plugin/plugin_schema_validator.hpp"

#include "core/schema/schema_validator.hpp"
#include "hosts/plugin/plugin_host.hpp"

namespace agentos {

std::string PluginOutputSchemaError(const PluginSpec& spec, const std::string& output_json) {
    const auto validation = ValidateCapabilityOutput(
        SkillManifest{
            .name = spec.name,
            .version = spec.manifest_version,
            .description = spec.description,
            .input_schema_json = spec.input_schema_json,
            .output_schema_json = spec.output_schema_json,
            .risk_level = spec.risk_level,
            .permissions = spec.permissions,
        },
        output_json,
        "plugin output");
    return validation.valid ? "" : validation.message;
}

}  // namespace agentos
