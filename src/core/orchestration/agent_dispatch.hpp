#pragma once

#include "core/models.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

class AuditLogger;
class CancellationToken;
class MemoryManager;
class PolicyEngine;

struct AgentDispatchInput {
    TaskRequest task;
    std::shared_ptr<IAgentAdapter> agent;
    std::string agent_name;
    std::string agent_task_id;
    std::string objective;
    std::string context_json;
    std::string constraints_json;
    StringMap invocation_context;
    StringMap invocation_constraints;
    std::optional<std::string> resume_session_id;
    std::shared_ptr<CancellationToken> cancel;
    AgentEventCallback on_agent_event;
};

struct AgentDispatchResult {
    bool success = false;
    TaskStepRecord step;
    std::string structured_output_json;
    std::vector<AgentArtifact> artifacts;
    std::string error_code;
    std::string error_message;
    int duration_ms = 0;
    double effective_cost = 0.0;
};

AgentDispatchResult DispatchAgent(
    const AgentDispatchInput& input,
    PolicyEngine& policy_engine,
    AuditLogger& audit_logger,
    MemoryManager& memory_manager);

}  // namespace agentos
