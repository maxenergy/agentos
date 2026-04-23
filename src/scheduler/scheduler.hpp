#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

class AgentLoop;

struct ScheduledTask {
    std::string schedule_id;
    bool enabled = true;
    long long next_run_epoch_ms = 0;
    int interval_seconds = 0;
    int max_runs = 1;
    int run_count = 0;
    TaskRequest task;
};

struct SchedulerRunRecord {
    std::string schedule_id;
    TaskRunResult result;
    bool rescheduled = false;
};

class Scheduler {
public:
    explicit Scheduler(std::filesystem::path store_path);

    ScheduledTask save(ScheduledTask scheduled_task);
    bool remove(const std::string& schedule_id);
    [[nodiscard]] std::optional<ScheduledTask> find(const std::string& schedule_id) const;
    [[nodiscard]] std::vector<ScheduledTask> list() const;
    [[nodiscard]] std::vector<ScheduledTask> due(long long now_epoch_ms) const;
    static long long NowEpochMs();

    std::vector<SchedulerRunRecord> run_due(AgentLoop& loop, long long now_epoch_ms = NowEpochMs());

    [[nodiscard]] const std::filesystem::path& store_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<ScheduledTask> scheduled_tasks_;
};

}  // namespace agentos
