#include "hosts/plugin/plugin_host.hpp"

#include "core/schema/schema_validator.hpp"
#include "hosts/plugin/plugin_spec_utils.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {
namespace {

constexpr char kListDelimiter = ',';
using Json = nlohmann::ordered_json;

std::optional<Json> ParseJsonObject(const std::string& text) {
    try {
        auto parsed = Json::parse(text);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

std::optional<std::string> JsonStringField(const Json& object, const std::string& name) {
    const auto found = object.find(name);
    if (found != object.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return std::nullopt;
}

std::vector<std::string> JsonStringArrayField(const Json& object, const std::string& name) {
    const auto found = object.find(name);
    if (found == object.end() || !found->is_array()) {
        return {};
    }

    std::vector<std::string> values;
    for (const auto& value : *found) {
        if (value.is_string()) {
            values.push_back(value.get<std::string>());
        }
    }
    return values;
}

std::optional<bool> JsonBoolField(const Json& object, const std::string& name) {
    const auto found = object.find(name);
    if (found != object.end() && found->is_boolean()) {
        return found->get<bool>();
    }
    return std::nullopt;
}

std::optional<std::string> JsonSchemaField(const Json& object, const std::string& name) {
    const auto found = object.find(name);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (found->is_string()) {
        return found->get<std::string>();
    }
    if (found->is_object()) {
        return found->dump();
    }
    return std::nullopt;
}

struct PluginSpecParseResult {
    std::optional<PluginSpec> spec;
    std::string error_message;
};

bool ParseBool(const std::string& value, bool& output) {
    if (value == "true" || value == "1") {
        output = true;
        return true;
    }
    if (value == "false" || value == "0") {
        output = false;
        return true;
    }
    return false;
}

bool IsCommentOrEmpty(const std::string& line) {
    return line.empty() || line[0] == '#';
}

bool IsSupportedManifestVersion(const std::string& version) {
    return version == "plugin.v1";
}

std::string TrimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool AssignIntField(
    const std::vector<std::string>& fields,
    const std::size_t index,
    const std::string& name,
    const int fallback,
    const int minimum,
    int& output,
    std::string& error_message) {
    if (index >= fields.size() || fields[index].empty()) {
        output = fallback;
        return true;
    }
    if (!ParseStrictInt(fields[index], output)) {
        error_message = "invalid integer field " + name + ": " + fields[index];
        return false;
    }
    if (output < minimum) {
        error_message = "integer field " + name + " must be >= " + std::to_string(minimum);
        return false;
    }
    return true;
}

bool AssignSizeField(
    const std::vector<std::string>& fields,
    const std::size_t index,
    const std::string& name,
    const std::size_t fallback,
    std::size_t& output,
    std::string& error_message) {
    if (index >= fields.size() || fields[index].empty()) {
        output = fallback;
        return true;
    }
    if (!ParseStrictSize(fields[index], output)) {
        error_message = "invalid size field " + name + ": " + fields[index];
        return false;
    }
    return true;
}

bool AssignBoolField(
    const std::vector<std::string>& fields,
    const std::size_t index,
    const std::string& name,
    const bool fallback,
    bool& output,
    std::string& error_message) {
    if (index >= fields.size() || fields[index].empty()) {
        output = fallback;
        return true;
    }
    if (!ParseBool(fields[index], output)) {
        error_message = "invalid boolean field " + name + ": " + fields[index];
        return false;
    }
    return true;
}

bool TemplateReferencesArgument(const std::vector<std::string>& args_template, const std::string& argument_name) {
    if (argument_name == "cwd") {
        return true;
    }

    const auto placeholder = "{{" + argument_name + "}}";
    return std::any_of(args_template.begin(), args_template.end(), [&placeholder](const std::string& value) {
        return value.find(placeholder) != std::string::npos;
    });
}

std::string RequiredArgTemplateError(const PluginSpec& spec) {
    for (const auto& required_arg : spec.required_args) {
        if (!TemplateReferencesArgument(spec.args_template, required_arg)) {
            return "required_arg is not referenced by args_template: " + required_arg;
        }
    }
    return {};
}

std::string HealthTemplatePlaceholderError(const PluginSpec& spec) {
    for (const auto& arg : spec.health_args_template) {
        std::size_t search_start = 0;
        while (true) {
            const auto open = arg.find("{{", search_start);
            if (open == std::string::npos) {
                break;
            }
            const auto close = arg.find("}}", open + 2);
            if (close == std::string::npos) {
                break;
            }
            const auto name = arg.substr(open + 2, close - open - 2);
            if (name != "cwd") {
                return "health_args_template references runtime argument: " + name;
            }
            search_start = close + 2;
        }
    }
    return {};
}

SkillManifest CapabilityManifestFromPluginSpec(const PluginSpec& spec) {
    return {
        .name = spec.name,
        .version = spec.manifest_version,
        .description = spec.description,
        .input_schema_json = spec.input_schema_json,
        .output_schema_json = spec.output_schema_json,
        .risk_level = spec.risk_level,
        .permissions = spec.permissions,
        .timeout_ms = spec.timeout_ms,
    };
}

std::string PluginCapabilityDeclarationError(const PluginSpec& spec) {
    const auto validation = ValidateCapabilityDeclaration(CapabilityManifestFromPluginSpec(spec));
    if (validation.valid) {
        return {};
    }
    if (validation.diagnostics.empty()) {
        return validation.message;
    }
    const auto& diagnostic = validation.diagnostics.front();
    if (diagnostic.field == "risk_level") {
        return "unsupported risk_level: " + spec.risk_level;
    }
    if (diagnostic.field == "permissions") {
        return "unknown permissions: " + diagnostic.constraint;
    }
    if (diagnostic.field == "input_schema_json") {
        return "input_schema_json must be a parseable JSON object";
    }
    if (diagnostic.field == "output_schema_json") {
        return "output_schema_json must be a parseable JSON object";
    }
    return validation.message;
}

PluginSpecParseResult ValidatePluginSpec(PluginSpec spec) {
    if (const auto declaration_error = PluginCapabilityDeclarationError(spec); !declaration_error.empty()) {
        return {
            .spec = std::nullopt,
            .error_message = declaration_error,
        };
    }
    if (!PluginSpecIsSupported(spec)) {
        return {
            .spec = std::nullopt,
            .error_message = PluginSpecUnsupportedReason(spec),
        };
    }
    if (const auto required_arg_error = RequiredArgTemplateError(spec); !required_arg_error.empty()) {
        return {
            .spec = std::nullopt,
            .error_message = required_arg_error,
        };
    }
    if (const auto health_arg_error = HealthTemplatePlaceholderError(spec); !health_arg_error.empty()) {
        return {
            .spec = std::nullopt,
            .error_message = health_arg_error,
        };
    }
    return {
        .spec = std::move(spec),
        .error_message = {},
    };
}

PluginSpecParseResult ParsePluginSpecLine(const std::string& line) {
    auto fields = SplitTsvFields(line);
    if (fields.size() < 9) {
        return {
            .spec = std::nullopt,
            .error_message = "plugin spec requires at least 9 fields",
        };
    }

    std::string manifest_version = "plugin.v1";
    if (fields[0].find('.') != std::string::npos) {
        manifest_version = fields[0];
        fields.erase(fields.begin());
    }

    if (!IsSupportedManifestVersion(manifest_version) || fields.size() < 9) {
        return {
            .spec = std::nullopt,
            .error_message = !IsSupportedManifestVersion(manifest_version)
                ? "unsupported plugin manifest version: " + manifest_version
                : "plugin spec requires at least 9 fields",
        };
    }

    PluginSpec spec;
    spec.manifest_version = manifest_version;
    spec.name = fields[0];
    spec.description = fields[1];
    spec.binary = fields[2];
    spec.args_template = SplitNonEmpty(fields[3], kListDelimiter);
    spec.required_args = SplitNonEmpty(fields[4], kListDelimiter);
    spec.protocol = fields[5].empty() ? "stdio-json-v0" : fields[5];
    spec.input_schema_json = fields.size() >= 10 ? fields[9] : R"({"type":"object"})";
    spec.output_schema_json = fields.size() >= 11 ? fields[10] : R"({"type":"object"})";
    spec.risk_level = fields[6];
    spec.permissions = SplitNonEmpty(fields[7], kListDelimiter);
    spec.env_allowlist = fields.size() >= 13 ? SplitNonEmpty(fields[12], kListDelimiter) : std::vector<std::string>{};
    spec.health_args_template = fields.size() >= 19 ? SplitNonEmpty(fields[18], kListDelimiter) : std::vector<std::string>{};
    spec.sandbox_mode = fields.size() >= 21 && !fields[20].empty() ? fields[20] : "workspace";
    spec.lifecycle_mode = fields.size() >= 22 && !fields[21].empty() ? fields[21] : "oneshot";

    std::string field_error;
    if (!AssignIntField(fields, 8, "timeout_ms", 3000, 1, spec.timeout_ms, field_error) ||
        !AssignSizeField(fields, 11, "output_limit_bytes", 1024 * 1024, spec.output_limit_bytes, field_error) ||
        !AssignBoolField(fields, 13, "idempotent", true, spec.idempotent, field_error) ||
        !AssignSizeField(fields, 14, "memory_limit_bytes", 0, spec.memory_limit_bytes, field_error) ||
        !AssignIntField(fields, 15, "max_processes", 0, 0, spec.max_processes, field_error) ||
        !AssignIntField(fields, 16, "cpu_time_limit_seconds", 0, 0, spec.cpu_time_limit_seconds, field_error) ||
        !AssignIntField(fields, 17, "file_descriptor_limit", 0, 0, spec.file_descriptor_limit, field_error) ||
        !AssignIntField(fields, 19, "health_timeout_ms", spec.timeout_ms, 1, spec.health_timeout_ms, field_error) ||
        !AssignIntField(fields, 22, "startup_timeout_ms", spec.timeout_ms, 1, spec.startup_timeout_ms, field_error) ||
        !AssignIntField(fields, 23, "idle_timeout_ms", 30000, 1, spec.idle_timeout_ms, field_error) ||
        !AssignIntField(fields, 24, "pool_size", 1, 1, spec.pool_size, field_error)) {
        return {
            .spec = std::nullopt,
            .error_message = field_error,
        };
    }

    return ValidatePluginSpec(std::move(spec));
}

bool AssignJsonIntField(
    const Json& manifest,
    const std::string& name,
    const int fallback,
    const int minimum,
    int& output,
    std::string& error_message) {
    const auto found = manifest.find(name);
    if (found == manifest.end()) {
        output = fallback;
        return true;
    }
    if (!found->is_number_integer()) {
        error_message = "invalid integer field " + name + ": " + found->dump();
        return false;
    }
    output = found->get<int>();
    if (output < minimum) {
        error_message = "integer field " + name + " must be >= " + std::to_string(minimum);
        return false;
    }
    return true;
}

bool AssignJsonSizeField(
    const Json& manifest,
    const std::string& name,
    const std::size_t fallback,
    std::size_t& output,
    std::string& error_message) {
    const auto found = manifest.find(name);
    if (found == manifest.end()) {
        output = fallback;
        return true;
    }
    if (!found->is_number_integer()) {
        error_message = "invalid size field " + name + ": " + found->dump();
        return false;
    }
    const auto value = found->get<long long>();
    if (value < 0) {
        error_message = "invalid size field " + name + ": " + found->dump();
        return false;
    }
    output = static_cast<std::size_t>(value);
    return true;
}

PluginSpecParseResult ParsePluginSpecJsonManifest(const std::string& manifest) {
    const auto trimmed = TrimWhitespace(manifest);
    const auto parsed = ParseJsonObject(trimmed);
    if (!parsed.has_value()) {
        return {
            .spec = std::nullopt,
            .error_message = "plugin JSON manifest must be a JSON object",
        };
    }

    PluginSpec spec;
    if (auto value = JsonStringField(*parsed, "manifest_version"); value.has_value()) {
        spec.manifest_version = *value;
    }
    spec.name = JsonStringField(*parsed, "name").value_or("");
    spec.description = JsonStringField(*parsed, "description").value_or("");
    spec.binary = JsonStringField(*parsed, "binary").value_or("");
    spec.args_template = JsonStringArrayField(*parsed, "args_template");
    spec.required_args = JsonStringArrayField(*parsed, "required_args");
    spec.protocol = JsonStringField(*parsed, "protocol").value_or("stdio-json-v0");
    spec.input_schema_json = JsonSchemaField(*parsed, "input_schema_json").value_or(R"({"type":"object"})");
    spec.output_schema_json = JsonSchemaField(*parsed, "output_schema_json").value_or(R"({"type":"object"})");
    spec.risk_level = JsonStringField(*parsed, "risk_level").value_or("low");
    spec.permissions = JsonStringArrayField(*parsed, "permissions");
    spec.env_allowlist = JsonStringArrayField(*parsed, "env_allowlist");
    spec.idempotent = JsonBoolField(*parsed, "idempotent").value_or(true);
    spec.health_args_template = JsonStringArrayField(*parsed, "health_args_template");
    spec.sandbox_mode = JsonStringField(*parsed, "sandbox_mode").value_or("workspace");
    spec.lifecycle_mode = JsonStringField(*parsed, "lifecycle_mode").value_or("oneshot");

    std::string field_error;
    if (!AssignJsonIntField(*parsed, "timeout_ms", 3000, 1, spec.timeout_ms, field_error) ||
        !AssignJsonSizeField(*parsed, "output_limit_bytes", 1024 * 1024, spec.output_limit_bytes, field_error) ||
        !AssignJsonSizeField(*parsed, "memory_limit_bytes", 0, spec.memory_limit_bytes, field_error) ||
        !AssignJsonIntField(*parsed, "max_processes", 0, 0, spec.max_processes, field_error) ||
        !AssignJsonIntField(*parsed, "cpu_time_limit_seconds", 0, 0, spec.cpu_time_limit_seconds, field_error) ||
        !AssignJsonIntField(*parsed, "file_descriptor_limit", 0, 0, spec.file_descriptor_limit, field_error) ||
        !AssignJsonIntField(*parsed, "health_timeout_ms", spec.timeout_ms, 1, spec.health_timeout_ms, field_error) ||
        !AssignJsonIntField(*parsed, "startup_timeout_ms", spec.timeout_ms, 1, spec.startup_timeout_ms, field_error) ||
        !AssignJsonIntField(*parsed, "idle_timeout_ms", 30000, 1, spec.idle_timeout_ms, field_error) ||
        !AssignJsonIntField(*parsed, "pool_size", 1, 1, spec.pool_size, field_error)) {
        return {
            .spec = std::nullopt,
            .error_message = field_error,
        };
    }

    return ValidatePluginSpec(std::move(spec));
}

void AppendLoadedPluginSpec(
    PluginLoadResult& result,
    std::set<std::string>& loaded_names,
    PluginSpec spec,
    const std::filesystem::path& file,
    const int line_number) {
    spec.source_file = file;
    spec.source_line_number = line_number;
    if (loaded_names.contains(spec.name)) {
        result.diagnostics.push_back(PluginLoadDiagnostic{
            .file = file,
            .line_number = line_number,
            .reason = "duplicate plugin spec name: " + spec.name,
        });
        return;
    }
    loaded_names.insert(spec.name);
    result.specs.push_back(std::move(spec));
}

}  // namespace

PluginLoadResult LoadPluginSpecsWithDiagnostics(const std::filesystem::path& spec_dir) {
    PluginLoadResult result;
    if (spec_dir.empty() || !std::filesystem::exists(spec_dir)) {
        return result;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(spec_dir)) {
        if (entry.is_regular_file() && (entry.path().extension() == ".tsv" || entry.path().extension() == ".json")) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::set<std::string> loaded_names;
    for (const auto& file : files) {
        if (file.extension() == ".json") {
            std::ifstream input(file, std::ios::binary);
            const std::string manifest((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            const auto parsed = ParsePluginSpecJsonManifest(manifest);
            if (parsed.spec.has_value()) {
                AppendLoadedPluginSpec(result, loaded_names, *parsed.spec, file, 1);
            } else {
                result.diagnostics.push_back(PluginLoadDiagnostic{
                    .file = file,
                    .line_number = 1,
                    .reason = parsed.error_message.empty() ? "invalid plugin JSON manifest" : parsed.error_message,
                });
            }
        } else {
            std::ifstream input(file, std::ios::binary);
            std::string line;
            int line_number = 0;
            while (std::getline(input, line)) {
                ++line_number;
                if (IsCommentOrEmpty(line)) {
                    continue;
                }
                const auto parsed = ParsePluginSpecLine(line);
                if (parsed.spec.has_value()) {
                    AppendLoadedPluginSpec(result, loaded_names, *parsed.spec, file, line_number);
                } else {
                    result.diagnostics.push_back(PluginLoadDiagnostic{
                        .file = file,
                        .line_number = line_number,
                        .reason = parsed.error_message.empty() ? "invalid plugin spec" : parsed.error_message,
                    });
                }
            }
        }
    }

    return result;
}

std::vector<PluginSpec> LoadPluginSpecsFromDirectory(const std::filesystem::path& spec_dir) {
    return LoadPluginSpecsWithDiagnostics(spec_dir).specs;
}

}  // namespace agentos
