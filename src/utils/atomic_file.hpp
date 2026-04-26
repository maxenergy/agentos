#pragma once

#include <filesystem>
#include <string_view>

namespace agentos {

void AppendLineToFile(const std::filesystem::path& path, std::string_view line);
void WriteFileAtomically(const std::filesystem::path& path, std::string_view content);

}  // namespace agentos
