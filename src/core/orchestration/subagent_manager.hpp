#pragma once

#include "core/audit/audit_logger.hpp"
#include "core/models.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "memory/memory_manager.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace agentos {

class CancellationToken;

enum class SubagentExecutionMode {
    sequential,
    parallel,
};

std::string ToString(SubagentExecutionMode mode);
SubagentExecutionMode ParseSubagentExecutionMode(const std::string& value);

class SubagentManager {
public:
    SubagentManager(
        AgentRegistry& agent_registry,
        PolicyEngine& policy_engine,
        AuditLogger& audit_logger,
        MemoryManager& memory_manager,
        std::size_t max_subagents = 4,
        std::size_t max_parallel_subagents = 4,
        double max_estimated_cost = 0.0);

    // `cancel` (if non-null) is forwarded into each V2 AgentInvocation and
    // checked before per-agent dispatch so the orchestrator can interrupt a
    // batch from outside. Legacy adapters honor it on the pre-dispatch check
    // only — they have no in-flight cancellation hook.
    TaskRunResult run(
        const TaskRequest& task,
        const std::vector<std::string>& agent_names,
        SubagentExecutionMode mode = SubagentExecutionMode::sequential,
        std::shared_ptr<CancellationToken> cancel = {});

private:
    std::vector<std::string> select_agent_candidates(const TaskRequest& task) const;
    TaskStepRecord run_one(
        const TaskRequest& task,
        const std::string& agent_name,
        const std::string& role,
        const std::shared_ptr<CancellationToken>& cancel) const;

    AgentRegistry& agent_registry_;
    PolicyEngine& policy_engine_;
    AuditLogger& audit_logger_;
    MemoryManager& memory_manager_;
    std::size_t max_subagents_ = 4;
    std::size_t max_parallel_subagents_ = 4;
    double max_estimated_cost_ = 0.0;
};

}  // namespace agentos
