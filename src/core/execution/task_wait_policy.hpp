#pragma once

#include "core/models.hpp"

#include <string>

namespace agentos {

enum class TaskWaitPolicyKind {
    chat,
    research,
    development,
};

struct TaskWaitPolicy {
    std::string name;
    int idle_timeout_ms = 0;
    int soft_deadline_ms = 0;
    int hard_deadline_ms = 0;
    int heartbeat_interval_ms = 0;
};

TaskWaitPolicy ResolveTaskWaitPolicy(TaskWaitPolicyKind kind);

void ApplyTaskWaitPolicy(TaskRequest& task, const TaskWaitPolicy& policy);

}  // namespace agentos
