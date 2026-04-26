#include "hosts/plugin/plugin_host.hpp"

#include "hosts/plugin/plugin_json_utils.hpp"
#include "hosts/plugin/plugin_spec_utils.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <utility>

namespace agentos {
namespace {

constexpr char kListDelimiter = ',';

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

PluginSpecParseResult ValidatePluginSpec(PluginSpec spec) {
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
    if (!IsLikelyJsonObjectString(spec.input_schema_json)) {
        return {
            .spec = std::nullopt,
            .error_message = "input_schema_json must be a JSON object",
        };
    }
    if (!IsLikelyJsonObjectString(spec.output_schema_json)) {
        return {
            .spec = std::nullopt,
            .error_message = "output_schema_json must be a JSON object",
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
    const std::string& manifest,
    const std::string& name,
    const int fallback,
    const int minimum,
    int& output,
    std::string& error_message) {
    const auto token = JsonNumberTokenField(manifest, name);
    if (!token.has_value()) {
        output = fallback;
        return true;
    }
    if (!ParseStrictInt(*token, output)) {
        error_message = "invalid integer field " + name + ": " + *token;
        return false;
    }
    if (output < minimum) {
        error_message = "integer field " + name + " must be >= " + std::to_string(minimum);
        return false;
    }
    return true;
}

bool AssignJsonSizeField(
    const std::string& manifest,
    const std::string& name,
    const std::size_t fallback,
    std::size_t& output,
    std::string& error_message) {
    const auto token = JsonNumberTokenField(manifest, name);
    if (!token.has_value()) {
        output = fallback;
        return true;
    }
    if (!ParseStrictSize(*token, output)) {
        error_message = "invalid size field " + name + ": " + *token;
        return false;
    }
    return true;
}

PluginSpecParseResult ParsePluginSpecJsonManifest(const std::string& manifest) {
    const auto trimmed = TrimWhitespace(manifest);
    if (!IsLikelyJsonObjectString(trimmed)) {
        return {
            .spec = std::nullopt,
            .error_message = "plugin JSON manifest must be a JSON object",
        };
    }

    PluginSpec spec;
    if (auto value = JsonStringField(trimmed, "manifest_version"); value.has_value()) {
        spec.manifest_version = *value;
    }
    spec.name = JsonStringField(trimmed, "name").value_or("");
    spec.description = JsonStringField(trimmed, "description").value_or("");
    spec.binary = JsonStringField(trimmed, "binary").value_or("");
    spec.args_template = JsonStringArrayField(trimmed, "args_template").value_or(std::vector<std::string>{});
    spec.required_args = JsonStringArrayField(trimmed, "required_args").value_or(std::vector<std::string>{});
    spec.protocol = JsonStringField(trimmed, "protocol").value_or("stdio-json-v0");
    spec.input_schema_json = JsonSchemaField(trimmed, "input_schema_json").value_or(R"({"type":"object"})");
    spec.output_schema_json = JsonSchemaField(trimmed, "output_schema_json").value_or(R"({"type":"object"})");
    spec.risk_level = JsonStringField(trimmed, "risk_level").value_or("low");
    spec.permissions = JsonStringArrayField(trimmed, "permissions").value_or(std::vector<std::string>{});
    spec.env_allowlist = JsonStringArrayField(trimmed, "env_allowlist").value_or(std::vector<std::string>{});
    spec.idempotent = JsonBoolField(trimmed, "idempotent").value_or(true);
    spec.health_args_template =
        JsonStringArrayField(trimmed, "health_args_template").value_or(std::vector<std::string>{});
    spec.sandbox_mode = JsonStringField(trimmed, "sandbox_mode").value_or("workspace");
    spec.lifecycle_mode = JsonStringField(trimmed, "lifecycle_mode").value_or("oneshot");

    std::string field_error;
    if (!AssignJsonIntField(trimmed, "timeout_ms", 3000, 1, spec.timeout_ms, field_error) ||
        !AssignJsonSizeField(trimmed, "output_limit_bytes", 1024 * 1024, spec.output_limit_bytes, field_error) ||
        !AssignJsonSizeField(trimmed, "memory_limit_bytes", 0, spec.memory_limit_bytes, field_error) ||
        !AssignJsonIntField(trimmed, "max_processes", 0, 0, spec.max_processes, field_error) ||
        !AssignJsonIntField(trimmed, "cpu_time_limit_seconds", 0, 0, spec.cpu_time_limit_seconds, field_error) ||
        !AssignJsonIntField(trimmed, "file_descriptor_limit", 0, 0, spec.file_descriptor_limit, field_error) ||
        !AssignJsonIntField(trimmed, "health_timeout_ms", spec.timeout_ms, 1, spec.health_timeout_ms, field_error) ||
        !AssignJsonIntField(trimmed, "startup_timeout_ms", spec.timeout_ms, 1, spec.startup_timeout_ms, field_error) ||
        !AssignJsonIntField(trimmed, "idle_timeout_ms", 30000, 1, spec.idle_timeout_ms, field_error) ||
        !AssignJsonIntField(trimmed, "pool_size", 1, 1, spec.pool_size, field_error)) {
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
