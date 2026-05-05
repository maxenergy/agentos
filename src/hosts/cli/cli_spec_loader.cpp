#include "hosts/cli/cli_spec_loader.hpp"

#include "core/schema/schema_validator.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <set>

namespace agentos {

namespace {

constexpr char kFieldDelimiter = '\t';
constexpr char kListDelimiter = ',';

bool IsCommentOrEmpty(const std::string& line) {
    return line.empty() || line[0] == '#';
}

bool IsSupportedParseMode(const std::string& parse_mode) {
    return parse_mode == "text" || parse_mode == "json" || parse_mode == "json_lines";
}


struct CliSpecParseResult {
    std::optional<CliSpec> spec;
    std::string error_message;
};

SkillManifest CapabilityManifestFromCliSpec(const CliSpec& spec) {
    return {
        .name = spec.name,
        .description = spec.description,
        .input_schema_json = spec.input_schema_json,
        .output_schema_json = spec.output_schema_json,
        .risk_level = spec.risk_level,
        .permissions = spec.permissions,
        .timeout_ms = spec.timeout_ms,
    };
}

std::string CliCapabilityDeclarationError(const CliSpec& spec) {
    const auto validation = ValidateCapabilityDeclaration(CapabilityManifestFromCliSpec(spec));
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

bool TemplateReferencesArgument(const std::vector<std::string>& args_template, const std::string& argument_name) {
    if (argument_name == "cwd") {
        return true;
    }

    const auto placeholder = "{{" + argument_name + "}}";
    return std::any_of(args_template.begin(), args_template.end(), [&placeholder](const std::string& value) {
        return value.find(placeholder) != std::string::npos;
    });
}

std::string RequiredArgTemplateError(const CliSpec& spec) {
    for (const auto& required_arg : spec.required_args) {
        if (!TemplateReferencesArgument(spec.args_template, required_arg)) {
            return "required_arg is not referenced by args_template: " + required_arg;
        }
    }
    return {};
}

CliSpecParseResult ParseCliSpecLine(const std::string& line) {
    const auto fields = SplitTsvFields(line);
    if (fields.size() < 9) {
        return {
            .spec = std::nullopt,
            .error_message = "CLI spec requires at least 9 fields",
        };
    }

    CliSpec spec;
    spec.name = fields[0];
    spec.description = fields[1];
    spec.binary = fields[2];
    spec.args_template = SplitNonEmpty(fields[3], kListDelimiter);
    spec.required_args = SplitNonEmpty(fields[4], kListDelimiter);
    spec.input_schema_json = fields.size() >= 10 ? fields[9] : R"({"type":"object"})";
    spec.output_schema_json = fields.size() >= 11 ? fields[10] : R"({"type":"object","required":["stdout","stderr","exit_code"]})";
    spec.parse_mode = fields[5];
    spec.risk_level = fields[6];
    spec.permissions = SplitNonEmpty(fields[7], kListDelimiter);
    spec.env_allowlist = fields.size() >= 13 ? SplitNonEmpty(fields[12], kListDelimiter) : std::vector<std::string>{};

    std::string numeric_error;
    if (!AssignIntField(fields, 8, "timeout_ms", 3000, 1, spec.timeout_ms, numeric_error) ||
        !AssignSizeField(fields, 11, "output_limit_bytes", 1024 * 1024, spec.output_limit_bytes, numeric_error) ||
        !AssignSizeField(fields, 13, "memory_limit_bytes", 0, spec.memory_limit_bytes, numeric_error) ||
        !AssignIntField(fields, 14, "max_processes", 0, 0, spec.max_processes, numeric_error) ||
        !AssignIntField(fields, 15, "cpu_time_limit_seconds", 0, 0, spec.cpu_time_limit_seconds, numeric_error) ||
        !AssignIntField(fields, 16, "file_descriptor_limit", 0, 0, spec.file_descriptor_limit, numeric_error)) {
        return {
            .spec = std::nullopt,
            .error_message = numeric_error,
        };
    }

    if (spec.name.empty() || spec.binary.empty()) {
        return {
            .spec = std::nullopt,
            .error_message = spec.name.empty() ? "CLI spec name is required" : "CLI spec binary is required",
        };
    }
    if (!IsSupportedParseMode(spec.parse_mode)) {
        return {
            .spec = std::nullopt,
            .error_message = "unsupported CLI parse_mode: " + spec.parse_mode,
        };
    }
    if (const auto required_arg_error = RequiredArgTemplateError(spec); !required_arg_error.empty()) {
        return {
            .spec = std::nullopt,
            .error_message = required_arg_error,
        };
    }
    if (const auto declaration_error = CliCapabilityDeclarationError(spec); !declaration_error.empty()) {
        return {
            .spec = std::nullopt,
            .error_message = declaration_error,
        };
    }
    return {
        .spec = std::move(spec),
        .error_message = {},
    };
}

}  // namespace

CliSpecLoadResult LoadCliSpecsWithDiagnostics(const std::filesystem::path& spec_dir) {
    CliSpecLoadResult result;
    if (spec_dir.empty() || !std::filesystem::exists(spec_dir)) {
        return result;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(spec_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tsv") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::set<std::string> loaded_names;
    for (const auto& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string line;
        int line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (IsCommentOrEmpty(line)) {
                continue;
            }
            const auto parsed = ParseCliSpecLine(line);
            if (parsed.spec.has_value()) {
                auto spec = *parsed.spec;
                spec.source_file = file;
                spec.source_line_number = line_number;
                if (loaded_names.contains(spec.name)) {
                    result.diagnostics.push_back(CliSpecLoadDiagnostic{
                        .file = file,
                        .line_number = line_number,
                        .reason = "duplicate CLI spec name: " + spec.name,
                    });
                    continue;
                }
                loaded_names.insert(spec.name);
                result.specs.push_back(std::move(spec));
            } else {
                result.diagnostics.push_back(CliSpecLoadDiagnostic{
                    .file = file,
                    .line_number = line_number,
                    .reason = parsed.error_message.empty() ? "invalid CLI spec" : parsed.error_message,
                });
            }
        }
    }

    return result;
}

std::vector<CliSpec> LoadCliSpecsFromDirectory(const std::filesystem::path& spec_dir) {
    return LoadCliSpecsWithDiagnostics(spec_dir).specs;
}

}  // namespace agentos
