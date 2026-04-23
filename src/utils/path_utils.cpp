#include "utils/path_utils.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace agentos {

namespace {

std::string ComparablePathString(const std::filesystem::path& path) {
    auto value = path.lexically_normal().generic_string();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return value;
}

}  // namespace

std::filesystem::path NormalizeWorkspaceRoot(const std::filesystem::path& workspace_root) {
    return std::filesystem::absolute(workspace_root).lexically_normal();
}

std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& workspace_root, const std::string& candidate_path) {
    const auto normalized_root = NormalizeWorkspaceRoot(workspace_root);
    const std::filesystem::path candidate(candidate_path);

    if (candidate.is_absolute()) {
        return candidate.lexically_normal();
    }

    return (normalized_root / candidate).lexically_normal();
}

bool IsPathInsideWorkspace(const std::filesystem::path& workspace_root, const std::filesystem::path& candidate_path) {
    const auto normalized_root = NormalizeWorkspaceRoot(workspace_root);
    const auto comparable_root = ComparablePathString(normalized_root);
    auto comparable_candidate = ComparablePathString(candidate_path);

    if (comparable_candidate == comparable_root) {
        return true;
    }

    auto root_prefix = comparable_root;
    if (!root_prefix.empty() && root_prefix.back() != '/') {
        root_prefix.push_back('/');
    }

    return comparable_candidate.rfind(root_prefix, 0) == 0;
}

}  // namespace agentos
