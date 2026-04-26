#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace agentos {

int RunCliSpecsCommand(
    const std::filesystem::path& workspace,
    const std::set<std::string>& conflict_names,
    int argc,
    char* argv[]);

}  // namespace agentos
