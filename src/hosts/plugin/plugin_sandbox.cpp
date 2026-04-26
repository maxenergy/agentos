#include "hosts/plugin/plugin_sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace agentos {

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ContainsPathLikeName(const std::string& name) {
    const auto lower = ToLowerAscii(name);
    return lower == "cwd" ||
           lower.find("path") != std::string::npos ||
           lower.find("file") != std::string::npos ||
           lower.find("dir") != std::string::npos;
}

std::filesystem::path NormalizeForSandbox(const std::filesystem::path& path) {
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        normalized = std::filesystem::absolute(path, error);
        if (error) {
            normalized = path;
        }
    }
    return normalized.lexically_normal();
}

bool PathInsideOrEqual(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_text = NormalizeForSandbox(root).string();
    auto candidate_text = NormalizeForSandbox(candidate).string();
#ifdef _WIN32
    root_text = ToLowerAscii(root_text);
    candidate_text = ToLowerAscii(candidate_text);
#endif
    if (candidate_text == root_text) {
        return true;
    }
    if (!root_text.empty() && root_text.back() != std::filesystem::path::preferred_separator) {
        root_text.push_back(std::filesystem::path::preferred_separator);
    }
    return candidate_text.rfind(root_text, 0) == 0;
}

std::filesystem::path ResolveSandboxArgumentPath(
    const std::filesystem::path& workspace_path,
    const std::string& value) {
    const std::filesystem::path value_path(value);
    if (value_path.is_absolute()) {
        return value_path;
    }
    return workspace_path / value_path;
}

}  // namespace

std::string PluginSandboxError(const PluginRunRequest& request) {
    if (request.spec.sandbox_mode == "none") {
        return {};
    }
    if (request.spec.sandbox_mode != "workspace") {
        return "unsupported sandbox_mode: " + request.spec.sandbox_mode;
    }
    if (request.workspace_path.empty()) {
        return "workspace sandbox requires a workspace_path";
    }

    for (const auto& [name, value] : request.arguments) {
        if (!ContainsPathLikeName(name) || value.empty()) {
            continue;
        }
        const auto resolved = ResolveSandboxArgumentPath(request.workspace_path, value);
        if (!PathInsideOrEqual(request.workspace_path, resolved)) {
            return "plugin sandbox denied path argument outside workspace: " + name;
        }
    }
    return {};
}

}  // namespace agentos
