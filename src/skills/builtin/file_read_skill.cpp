#include "skills/builtin/file_read_skill.hpp"

#include "utils/json_utils.hpp"
#include "utils/path_utils.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace agentos {

SkillManifest FileReadSkill::manifest() const {
    return {
        .name = "file_read",
        .version = "0.1.0",
        .description = "Read a text file inside the active workspace.",
        .capabilities = {"filesystem", "read"},
        .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
        .output_schema_json = R"({"type":"object","required":["path","content"]})",
        .risk_level = "low",
        .permissions = {"filesystem.read"},
        .supports_streaming = false,
        .idempotent = true,
        .timeout_ms = 2000,
    };
}

SkillResult FileReadSkill::execute(const SkillCall& call) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto maybe_path = call.get_arg("path");
    if (!maybe_path.has_value()) {
        return {false, "", "InvalidArguments", "path is required", 0};
    }

    const auto resolved_path = ResolveWorkspacePath(call.workspace_id, *maybe_path);
    std::ifstream input(resolved_path, std::ios::binary);
    if (!input) {
        return {false, "", "FileOpenFailed", "could not open file for reading", 0};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    return {
        .success = true,
        .json_output = MakeJsonObject({
            {"path", QuoteJson(resolved_path.string())},
            {"content", QuoteJson(buffer.str())},
        }),
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count()),
    };
}

bool FileReadSkill::healthy() const {
    return true;
}

}  // namespace agentos
