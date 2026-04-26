#pragma once

#include "hosts/cli/cli_host.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace agentos {

struct CliSpecLoadDiagnostic {
    std::filesystem::path file;
    int line_number = 0;
    std::string reason;
};

struct CliSpecLoadResult {
    std::vector<CliSpec> specs;
    std::vector<CliSpecLoadDiagnostic> diagnostics;
};

std::vector<CliSpec> LoadCliSpecsFromDirectory(const std::filesystem::path& spec_dir);
CliSpecLoadResult LoadCliSpecsWithDiagnostics(const std::filesystem::path& spec_dir);

}  // namespace agentos
