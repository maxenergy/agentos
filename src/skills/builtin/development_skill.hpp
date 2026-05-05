#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <functional>
#include <utility>

namespace agentos {

class AgentRegistry;
class AgentLoop;
class AuditLogger;

// Wraps the acceptance/repair loop the REPL used to run inline. Routing a
// `task_type=development_request` task through the agent loop hands control to
// this skill, which dispatches to the configured writable agent (codex_cli by
// default), reviews the deliverables manifest, and retries up to
// AGENTOS_DEV_MAX_ATTEMPTS times before reporting a structured result.
class DevelopmentSkill final : public ISkillAdapter {
public:
    using EventPrinter = std::function<void(const AgentEvent& event)>;

    DevelopmentSkill(AgentRegistry& agent_registry,
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

    // Installed by the REPL at startup so interactive runs still get the
    // live `[agent]` / `[tool]` lines on stdout. Non-interactive callers
    // (or callers that do not pass `interactive=true`) get a structured
    // JSON result instead.
    void set_event_printer(EventPrinter printer) { event_printer_ = std::move(printer); }

private:
    AgentRegistry& agent_registry_;
    AgentLoop& loop_;
    AuditLogger& audit_logger_;
    std::filesystem::path workspace_root_;
    EventPrinter event_printer_;
};

}  // namespace agentos
