#pragma once

#include "hosts/cli/cli_host.hpp"

#include <filesystem>
#include <vector>

namespace agentos {

std::vector<CliSpec> LoadCliSpecsFromDirectory(const std::filesystem::path& spec_dir);

}  // namespace agentos
