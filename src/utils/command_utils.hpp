#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace agentos {

std::optional<std::filesystem::path> ResolveCommandPath(const std::string& command);
bool CommandExists(const std::string& command);

}  // namespace agentos

