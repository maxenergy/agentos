#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace agentos {

class SkillRegistry;
class CliHost;
class PluginHost;
class AuditLogger;

std::unordered_set<std::string> ProtectedSkillNames();

// Reload external CLI specs and plugin specs from the workspace, registering
// new ones, unregistering removed ones, and recording diagnostics for any that
// conflict with protected runtime skills.
void ReloadExternalSkills(SkillRegistry& skill_registry,
                          CliHost& cli_host,
                          PluginHost& plugin_host,
                          AuditLogger& audit_logger,
                          const std::filesystem::path& workspace);

}  // namespace agentos
