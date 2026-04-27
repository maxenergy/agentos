#pragma once

#include "core/loop/agent_loop.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "core/audit/audit_logger.hpp"

#include <filesystem>

namespace agentos {

// HTTP JSON API server mode.  Listens on a configurable host:port and exposes
// REST endpoints for task execution, agent/skill listing, and status queries.
// Ctrl-C triggers a graceful shutdown.
//
//   agentos serve [port=18080] [host=127.0.0.1]
int RunServeCommand(
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    AgentLoop& loop,
    MemoryManager& memory_manager,
    Scheduler& scheduler,
    AuditLogger& audit_logger,
    const std::filesystem::path& workspace,
    int argc,
    char* argv[]);

}  // namespace agentos
