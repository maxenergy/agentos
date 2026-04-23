#pragma once

#include "core/models.hpp"
#include "memory/lesson_store.hpp"
#include "memory/workflow_store.hpp"

#include <filesystem>
#include <unordered_map>
#include <vector>

namespace agentos {

struct TaskMemoryRecord {
    std::string task_id;
    std::string task_type;
    std::string objective;
    std::string idempotency_key;
    std::vector<TaskStepRecord> steps;
    bool success = false;
    bool from_cache = false;
    int duration_ms = 0;
};

class MemoryManager {
public:
    MemoryManager() = default;
    explicit MemoryManager(std::filesystem::path storage_dir);

    void record_task(const TaskRequest& task, const TaskRunResult& result);

    [[nodiscard]] const std::vector<TaskMemoryRecord>& task_log() const;
    [[nodiscard]] const std::unordered_map<std::string, SkillStats>& skill_stats() const;
    [[nodiscard]] const std::unordered_map<std::string, AgentRuntimeStats>& agent_stats() const;
    [[nodiscard]] std::vector<WorkflowCandidate> workflow_candidates() const;
    [[nodiscard]] WorkflowStore& workflow_store();
    [[nodiscard]] const WorkflowStore& workflow_store() const;
    [[nodiscard]] LessonStore& lesson_store();
    [[nodiscard]] const LessonStore& lesson_store() const;
    void refresh_workflow_store() const;

private:
    static double UpdateAverage(double current_average, int previous_count, double sample);
    void load_persisted_logs();
    void append_task_log(const TaskRequest& task, const TaskRunResult& result) const;
    void append_step_log(const TaskRequest& task, const TaskStepRecord& step) const;
    void flush_stats() const;

    std::filesystem::path storage_dir_;
    LessonStore lesson_store_;
    WorkflowStore workflow_store_;
    std::vector<TaskMemoryRecord> tasks_;
    std::unordered_map<std::string, SkillStats> skill_stats_;
    std::unordered_map<std::string, AgentRuntimeStats> agent_stats_;
};

}  // namespace agentos
