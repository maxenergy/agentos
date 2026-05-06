#pragma once

#include "core/loop/agent_loop.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "core/audit/audit_logger.hpp"

#include <filesystem>

namespace agentos {

// Interactive REPL mode.  Reads commands from stdin in a loop, dispatching
// them through the existing Runtime components.  Ctrl-C interrupts a running
// task; a second Ctrl-C (or "exit"/"quit") terminates the loop.
int RunInteractiveCommand(
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    AgentLoop& loop,
    MemoryManager& memory_manager,
    Scheduler& scheduler,
    AuditLogger& audit_logger,
    const std::filesystem::path& workspace,
    int argc,
    char* argv[]);

// Run a chat task against `primary_target`; on failure walk the priority
// list gemini → anthropic → openai → qwen, skipping the primary and any
// adapter that is missing or `!healthy()`. Each attempt uses a 30s
// timeout. On full exhaustion, the returned result.error_message carries
// `tried=<csv>` so callers see the full chain. Exposed for tests.
TaskRunResult RunChatWithFallback(
    TaskRequest task,
    AgentRegistry& agent_registry,
    AgentLoop& loop,
    const std::string& primary_target);

}  // namespace agentos
