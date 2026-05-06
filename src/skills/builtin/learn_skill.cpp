#include "skills/builtin/learn_skill.hpp"

#include "cli/skill_reload.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/schema/schema_validator.hpp"
#include "utils/atomic_file.hpp"

#include <chrono>
#include <regex>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

bool IsValidSkillName(const std::string& name) {
    static const std::regex pattern("^[A-Za-z0-9_]+$");
    return !name.empty() && std::regex_match(name, pattern);
}

// CLI spec TSV column order matches cli_spec_loader.cpp::ParseCliSpecLine.
std::string BuildCliSpecLine(const std::string& name,
                             const std::string& description,
                             const std::string& binary,
                             const std::string& args_template,
                             const std::string& required_args,
                             const std::string& parse_mode,
                             const std::string& risk_level,
                             const std::string& permissions,
                             int timeout_ms,
                             const std::string& input_schema_json,
                             const std::string& output_schema_json,
                             std::size_t output_limit_bytes) {
    std::ostringstream out;
    out << name << '\t'
        << description << '\t'
        << binary << '\t'
        << args_template << '\t'
        << required_args << '\t'
        << parse_mode << '\t'
        << risk_level << '\t'
        << permissions << '\t'
        << timeout_ms << '\t'
        << input_schema_json << '\t'
        << output_schema_json << '\t'
        << output_limit_bytes
        << '\n';
    return out.str();
}

bool IsSupportedRiskLevelString(const std::string& value) {
    return value == "low" || value == "medium" || value == "high";
}

SkillManifest BuildLearnedCapabilityDeclaration(const std::string& name,
                                                const std::string& description,
                                                const std::string& risk_level,
                                                const int timeout_ms,
                                                const std::string& input_schema_json,
                                                const std::string& output_schema_json) {
    return {
        .name = name,
        .version = "0.1.0",
        .description = description,
        .capabilities = {"learned_cli"},
        .input_schema_json = input_schema_json,
        .output_schema_json = output_schema_json,
        .risk_level = risk_level,
        .permissions = {"process.spawn"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = timeout_ms,
    };
}

}  // namespace

SkillManifest LearnSkill::manifest() const {
    return {
        .name = "learn_skill",
        .version = "0.1.0",
        .description = "Register a new CLI skill at runtime by writing a spec under runtime/cli_specs/learned/.",
        .capabilities = {"self_extension"},
        .input_schema_json =
            R"({"type":"object","properties":{"name":{"type":"string"},"binary":{"type":"string"},"args_template":{"type":"string"},"required_args":{"type":"string"},"description":{"type":"string"},"risk_level":{"type":"string"},"timeout_ms":{"type":"string"},"from_url":{"type":"string"}},"required":["name"]})",
        .output_schema_json = R"({"type":"object","required":["registered","skill_name","spec_path"]})",
        .risk_level = "medium",
        .permissions = {"filesystem.write"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = 5000,
    };
}

SkillResult LearnSkill::execute(const SkillCall& call) {
    const auto started_at = std::chrono::steady_clock::now();

    const auto duration_ms = [&started_at]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count());
    };

    const auto maybe_name = call.get_arg("name");
    if (!maybe_name.has_value() || maybe_name->empty()) {
        return {false, "", "SchemaValidationFailed", "name is required", duration_ms()};
    }
    const std::string name = *maybe_name;

    if (const auto maybe_from_url = call.get_arg("from_url"); maybe_from_url.has_value() && !maybe_from_url->empty()) {
        return {false, "", "NotImplemented",
                "learn_skill from_url branch is not yet implemented; pass binary and args_template instead",
                duration_ms()};
    }

    if (!IsValidSkillName(name)) {
        return {false, "", "SchemaValidationFailed",
                "name must match [A-Za-z0-9_]+: " + name, duration_ms()};
    }

    const auto protected_names = ProtectedSkillNames();
    if (protected_names.contains(name)) {
        return {false, "", "SchemaValidationFailed",
                "name conflicts with a protected runtime skill: " + name, duration_ms()};
    }
    if (skill_registry_.find(name)) {
        return {false, "", "SchemaValidationFailed",
                "name conflicts with an already registered skill: " + name, duration_ms()};
    }

    const auto maybe_binary = call.get_arg("binary");
    const auto maybe_args_template = call.get_arg("args_template");
    if (!maybe_binary.has_value() || maybe_binary->empty()) {
        return {false, "", "SchemaValidationFailed", "binary is required", duration_ms()};
    }
    if (!maybe_args_template.has_value()) {
        return {false, "", "SchemaValidationFailed", "args_template is required", duration_ms()};
    }

    const std::string description = call.get_arg("description").value_or("Learned skill registered at runtime via learn_skill.");
    const std::string required_args = call.get_arg("required_args").value_or("");
    std::string risk_level = call.get_arg("risk_level").value_or("medium");
    if (!IsSupportedRiskLevelString(risk_level)) {
        return {false, "", "SchemaValidationFailed",
                "risk_level must be one of low|medium|high: " + risk_level, duration_ms()};
    }

    int timeout_ms = 30000;
    if (const auto maybe_timeout = call.get_arg("timeout_ms"); maybe_timeout.has_value() && !maybe_timeout->empty()) {
        try {
            timeout_ms = std::stoi(*maybe_timeout);
        } catch (...) {
            return {false, "", "SchemaValidationFailed",
                    "timeout_ms must be a positive integer: " + *maybe_timeout, duration_ms()};
        }
        if (timeout_ms < 1) {
            return {false, "", "SchemaValidationFailed",
                    "timeout_ms must be >= 1", duration_ms()};
        }
    }

    constexpr std::size_t kOutputLimitBytes = 131072;
    const std::string parse_mode = "text";
    const std::string permissions = "process.spawn";
    const std::string input_schema_json = R"({"type":"object"})";
    const std::string output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})";
    const auto declaration = BuildLearnedCapabilityDeclaration(
        name, description, risk_level, timeout_ms, input_schema_json, output_schema_json);
    if (const auto validation = ValidateCapabilityDeclaration(declaration); !validation.valid) {
        return {false, "", "SchemaValidationFailed", validation.message, duration_ms()};
    }

    const std::filesystem::path spec_dir = workspace_root_ / "runtime" / "cli_specs" / "learned";
    const std::filesystem::path spec_path = spec_dir / (name + ".tsv");
    const std::filesystem::path relative_spec_path =
        std::filesystem::path("runtime") / "cli_specs" / "learned" / (name + ".tsv");

    try {
        std::filesystem::create_directories(spec_dir);
        const auto line = BuildCliSpecLine(
            name, description, *maybe_binary, *maybe_args_template, required_args,
            parse_mode, risk_level, permissions, timeout_ms,
            input_schema_json, output_schema_json, kOutputLimitBytes);
        WriteFileAtomically(spec_path, line);
    } catch (const std::exception& ex) {
        return {false, "", "LearnFailed",
                std::string("could not write learned spec: ") + ex.what(), duration_ms()};
    }

    ReloadExternalSkills(skill_registry_, cli_host_, plugin_host_, audit_logger_, workspace_root_);

    if (!skill_registry_.find(name)) {
        return {false, "", "LearnFailed",
                "spec was written but skill_registry could not register it; check audit log for cli_spec diagnostics",
                duration_ms()};
    }

    nlohmann::json output;
    output["registered"] = true;
    output["skill_name"] = name;
    output["spec_path"] = relative_spec_path.generic_string();
    return {
        .success = true,
        .json_output = output.dump(),
        .duration_ms = duration_ms(),
    };
}

bool LearnSkill::healthy() const {
    return true;
}

}  // namespace agentos
