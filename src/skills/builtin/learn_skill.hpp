#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <utility>

namespace agentos {

class SkillRegistry;
class CliHost;
class PluginHost;
class AuditLogger;

class LearnSkill final : public ISkillAdapter {
public:
    LearnSkill(SkillRegistry& skill_registry,
               CliHost& cli_host,
               PluginHost& plugin_host,
               AuditLogger& audit_logger,
               std::filesystem::path workspace_root)
        : skill_registry_(skill_registry),
          cli_host_(cli_host),
          plugin_host_(plugin_host),
          audit_logger_(audit_logger),
          workspace_root_(std::move(workspace_root)) {}

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

private:
    SkillRegistry& skill_registry_;
    CliHost& cli_host_;
    PluginHost& plugin_host_;
    AuditLogger& audit_logger_;
    std::filesystem::path workspace_root_;
};

}  // namespace agentos
