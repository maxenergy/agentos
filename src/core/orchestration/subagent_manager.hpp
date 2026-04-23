#pragma once

#include "core/audit/audit_logger.hpp"
#include "core/models.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "memory/memory_manager.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace agentos {

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

    TaskRunResult run(
        const TaskRequest& task,
        const std::vector<std::string>& agent_names,
        SubagentExecutionMode mode = SubagentExecutionMode::sequential);

private:
    std::vector<std::string> select_agent_candidates(const TaskRequest& task) const;
    TaskStepRecord run_one(const TaskRequest& task, const std::string& agent_name) const;

    AgentRegistry& agent_registry_;
    PolicyEngine& policy_engine_;
    AuditLogger& audit_logger_;
    MemoryManager& memory_manager_;
    std::size_t max_subagents_ = 4;
    std::size_t max_parallel_subagents_ = 4;
    double max_estimated_cost_ = 0.0;
};

}  // namespace agentos
