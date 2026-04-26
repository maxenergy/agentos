#pragma once

#include "core/audit/audit_logger.hpp"
#include "core/models.hpp"
#include "memory/memory_manager.hpp"

#include <string>

namespace agentos {

void RecordTaskStep(AuditLogger& audit_logger, const std::string& task_id, const TaskStepRecord& step);
void FinalizeTaskRun(
    AuditLogger& audit_logger,
    MemoryManager& memory_manager,
    const TaskRequest& task,
    const TaskRunResult& result);

}  // namespace agentos
