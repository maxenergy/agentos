#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <functional>
#include <utility>

namespace agentos {

class AgentRegistry;
class AgentLoop;
class AuditLogger;

class ResearchSkill final : public ISkillAdapter {
public:
    using EventPrinter = std::function<void(const AgentEvent& event)>;

    ResearchSkill(AgentRegistry& agent_registry,
                  AgentLoop& loop,
                  AuditLogger& audit_logger,
                  std::filesystem::path workspace_root)
        : agent_registry_(agent_registry),
          loop_(loop),
          audit_logger_(audit_logger),
          workspace_root_(std::move(workspace_root)) {}

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

    // Optional context blob the REPL can pass through. Used to enrich the
    // research objective with the runtime's known agents/skills/tips so the
    // research agent has the same self-description we hand to chat.
    void set_runtime_guide(std::string guide) { runtime_guide_ = std::move(guide); }

    void set_event_printer(EventPrinter printer) { event_printer_ = std::move(printer); }

private:
    AgentRegistry& agent_registry_;
    AgentLoop& loop_;
    AuditLogger& audit_logger_;
    std::filesystem::path workspace_root_;
    std::string runtime_guide_;
    EventPrinter event_printer_;
};

}  // namespace agentos
