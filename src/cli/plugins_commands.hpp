#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace agentos {

std::string PluginSpecConflictReason(const std::string& name);

int RunPluginsCommand(
    const std::filesystem::path& workspace,
    const std::set<std::string>& builtin_skill_names,
    int argc,
    char* argv[]);

}  // namespace agentos
