#include "core/execution/task_lifecycle.hpp"

namespace agentos {

void RecordTaskStep(AuditLogger& audit_logger, const std::string& task_id, const TaskStepRecord& step) {
    audit_logger.record_step(task_id, step);
}

void FinalizeTaskRun(
    AuditLogger& audit_logger,
    MemoryManager& memory_manager,
    const TaskRequest& task,
    const TaskRunResult& result) {
    audit_logger.record_task_end(task.task_id, result);
    memory_manager.record_task(task, result);
}

}  // namespace agentos
