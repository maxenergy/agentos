#pragma once

#include <filesystem>

namespace agentos {

int RunAutoDevCommand(const std::filesystem::path& workspace, int argc, char* argv[]);

}  // namespace agentos
