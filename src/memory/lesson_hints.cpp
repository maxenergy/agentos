#include "memory/lesson_hints.hpp"

#include <sstream>

namespace agentos {

void ApplyLessonPolicyHint(
    const MemoryManager& memory_manager,
    const TaskRequest& task,
    const std::string& target_name,
    PolicyDecision& decision) {
    if (decision.allowed) {
        return;
    }

    int occurrences = 0;
    std::string summary;
    for (const auto& lesson : memory_manager.lesson_store().list()) {
        if (lesson.enabled &&
            lesson.task_type == task.task_type &&
            lesson.target_name == target_name &&
            lesson.error_code == "PolicyDenied") {
            occurrences += lesson.occurrence_count;
            if (summary.empty()) {
                summary = lesson.summary;
            }
        }
    }

    if (occurrences <= 0) {
        return;
    }

    std::ostringstream hint;
    hint << decision.reason
         << " lesson_hint=previous_policy_denials:"
         << occurrences;
    if (!summary.empty()) {
        hint << " last=\"" << summary << "\"";
    }
    decision.reason = hint.str();
}

}  // namespace agentos
