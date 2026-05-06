#include "core/execution/task_wait_policy.hpp"

namespace agentos {

TaskWaitPolicy ResolveTaskWaitPolicy(const TaskWaitPolicyKind kind) {
    switch (kind) {
    case TaskWaitPolicyKind::chat:
        return {
            .name = "interactive_chat",
            .idle_timeout_ms = 30000,
            .soft_deadline_ms = 30000,
            .hard_deadline_ms = 120000,
            .heartbeat_interval_ms = 5000,
        };
    case TaskWaitPolicyKind::research:
        return {
            .name = "interactive_research",
            .idle_timeout_ms = 60000,
            .soft_deadline_ms = 120000,
            .hard_deadline_ms = 600000,
            .heartbeat_interval_ms = 15000,
        };
    case TaskWaitPolicyKind::development:
        return {
            .name = "interactive_development",
            .idle_timeout_ms = 60000,
            .soft_deadline_ms = 180000,
            .hard_deadline_ms = 600000,
            .heartbeat_interval_ms = 15000,
        };
    }
    return {};
}

void ApplyTaskWaitPolicy(TaskRequest& task, const TaskWaitPolicy& policy) {
    task.timeout_ms = policy.hard_deadline_ms;
    task.inputs["wait_policy"] = policy.name;
    task.inputs["idle_timeout_ms"] = std::to_string(policy.idle_timeout_ms);
    task.inputs["soft_deadline_ms"] = std::to_string(policy.soft_deadline_ms);
    task.inputs["hard_deadline_ms"] = std::to_string(policy.hard_deadline_ms);
    task.inputs["heartbeat_interval_ms"] = std::to_string(policy.heartbeat_interval_ms);
}

}  // namespace agentos
