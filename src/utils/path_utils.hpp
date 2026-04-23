#pragma once

#include <filesystem>
#include <string>

namespace agentos {

std::filesystem::path NormalizeWorkspaceRoot(const std::filesystem::path& workspace_root);
std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& workspace_root, const std::string& candidate_path);
bool IsPathInsideWorkspace(const std::filesystem::path& workspace_root, const std::filesystem::path& candidate_path);

}  // namespace agentos

