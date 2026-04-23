#include "skills/builtin/file_write_skill.hpp"

#include "utils/json_utils.hpp"
#include "utils/path_utils.hpp"

#include <chrono>
#include <fstream>

namespace agentos {

SkillManifest FileWriteSkill::manifest() const {
    return {
        .name = "file_write",
        .version = "0.1.0",
        .description = "Write a text file inside the active workspace.",
        .capabilities = {"filesystem", "write"},
        .input_schema_json = R"({"type":"object","required":["path","content"]})",
        .output_schema_json = R"({"type":"object","required":["path","bytes_written"]})",
        .risk_level = "medium",
        .permissions = {"filesystem.write"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = 3000,
    };
}

SkillResult FileWriteSkill::execute(const SkillCall& call) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto maybe_path = call.get_arg("path");
    const auto maybe_content = call.get_arg("content");

    if (!maybe_path.has_value() || !maybe_content.has_value()) {
        return {false, "", "InvalidArguments", "path and content are required", 0};
    }

    const auto resolved_path = ResolveWorkspacePath(call.workspace_id, *maybe_path);
    if (!resolved_path.parent_path().empty()) {
        std::filesystem::create_directories(resolved_path.parent_path());
    }

    std::ios::openmode mode = std::ios::binary | std::ios::out;
    if (const auto maybe_mode = call.get_arg("mode"); maybe_mode.has_value() && *maybe_mode == "append") {
        mode |= std::ios::app;
    } else {
        mode |= std::ios::trunc;
    }

    std::ofstream output(resolved_path, mode);
    if (!output) {
        return {false, "", "FileOpenFailed", "could not open file for writing", 0};
    }

    output << *maybe_content;
    output.flush();

    return {
        .success = true,
        .json_output = MakeJsonObject({
            {"path", QuoteJson(resolved_path.string())},
            {"bytes_written", NumberAsJson(static_cast<long long>(maybe_content->size()))},
        }),
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count()),
    };
}

bool FileWriteSkill::healthy() const {
    return true;
}

}  // namespace agentos
