#include "skills/builtin/file_patch_skill.hpp"

#include "utils/path_utils.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

std::size_t ReplaceAll(std::string& content, const std::string& find, const std::string& replace) {
    if (find.empty()) {
        return 0;
    }

    std::size_t replacements = 0;
    std::size_t position = 0;

    while ((position = content.find(find, position)) != std::string::npos) {
        content.replace(position, find.size(), replace);
        position += replace.size();
        ++replacements;
    }

    return replacements;
}

}  // namespace

SkillManifest FilePatchSkill::manifest() const {
    return {
        .name = "file_patch",
        .version = "0.1.0",
        .description = "Apply a simple find/replace patch inside the active workspace.",
        .capabilities = {"filesystem", "patch"},
        .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"},"find":{"type":"string"},"replace":{"type":"string"}},"required":["path","find","replace"]})",
        .output_schema_json = R"({"type":"object","required":["path","replacements"]})",
        .risk_level = "medium",
        .permissions = {"filesystem.write"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = 3000,
    };
}

SkillResult FilePatchSkill::execute(const SkillCall& call) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto maybe_path = call.get_arg("path");
    const auto maybe_find = call.get_arg("find");
    const auto maybe_replace = call.get_arg("replace");

    if (!maybe_path.has_value() || !maybe_find.has_value() || !maybe_replace.has_value()) {
        return {false, "", "InvalidArguments", "path, find, and replace are required", 0};
    }

    const auto resolved_path = ResolveWorkspacePath(call.workspace_id, *maybe_path);
    std::ifstream input(resolved_path, std::ios::binary);
    if (!input) {
        return {false, "", "FileOpenFailed", "could not open file for patching", 0};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    auto content = buffer.str();

    const auto replacements = ReplaceAll(content, *maybe_find, *maybe_replace);
    if (replacements == 0) {
        return {false, "", "PatchTargetNotFound", "find text was not present in the target file", 0};
    }

    std::ofstream output(resolved_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return {false, "", "FileOpenFailed", "could not reopen file for patch write", 0};
    }

    output << content;
    output.flush();

    nlohmann::json output_json;
    output_json["path"] = resolved_path.string();
    output_json["replacements"] = static_cast<long long>(replacements);
    return {
        .success = true,
        .json_output = output_json.dump(),
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count()),
    };
}

bool FilePatchSkill::healthy() const {
    return true;
}

}  // namespace agentos
