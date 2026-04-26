#pragma once

#include "core/models.hpp"
#include "memory/memory_manager.hpp"

#include <string>

namespace agentos {

void ApplyLessonPolicyHint(
    const MemoryManager& memory_manager,
    const TaskRequest& task,
    const std::string& target_name,
    PolicyDecision& decision);

}  // namespace agentos
