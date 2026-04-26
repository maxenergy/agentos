#pragma once

#include "core/orchestration/subagent_manager.hpp"

#include <filesystem>

namespace agentos {

int RunSubagentsCommand(
    SubagentManager& subagent_manager,
    const std::filesystem::path& workspace,
    int argc,
    char* argv[]);

}  // namespace agentos
