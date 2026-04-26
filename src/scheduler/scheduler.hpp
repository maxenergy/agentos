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
    int max_retries = 0;
    int retry_count = 0;
    int retry_backoff_seconds = 0;
    std::string missed_run_policy = "run-once";
    // Optional five-field (or @alias) cron expression. When non-empty,
    // takes precedence over interval_seconds for computing the next run.
    std::string cron_expression;
    // IANA-style zone or fixed offset, e.g. "America/New_York", "UTC+08:00",
    // "UTC". Empty defaults to UTC. Used for cron evaluation; intervals are
    // timezone-insensitive (they are wall-clock-agnostic durations).
    std::string timezone_name;
    TaskRequest task;
};

struct SchedulerRunRecord {
    std::string schedule_id;
    TaskRunResult result;
    bool rescheduled = false;
};

struct SchedulerExecutionRecord {
    std::string schedule_id;
    std::string task_id;
    long long started_epoch_ms = 0;
    long long completed_epoch_ms = 0;
    int run_count = 0;
    bool success = false;
    bool rescheduled = false;
    std::string route_kind;
    std::string route_target;
    std::string error_code;
    std::string error_message;
    int duration_ms = 0;
};

class Scheduler {
public:
    explicit Scheduler(std::filesystem::path store_path);

    ScheduledTask save(ScheduledTask scheduled_task);
    bool remove(const std::string& schedule_id);
    [[nodiscard]] std::optional<ScheduledTask> find(const std::string& schedule_id) const;
    [[nodiscard]] std::vector<ScheduledTask> list() const;
    [[nodiscard]] std::vector<ScheduledTask> due(long long now_epoch_ms) const;
    [[nodiscard]] std::vector<SchedulerExecutionRecord> run_history() const;
    static long long NowEpochMs();

    std::vector<SchedulerRunRecord> run_due(AgentLoop& loop, long long now_epoch_ms = NowEpochMs());

    [[nodiscard]] const std::filesystem::path& store_path() const;
    [[nodiscard]] const std::filesystem::path& history_path() const;

private:
    void load();
    void flush() const;
    void append_execution_record(const SchedulerExecutionRecord& record) const;

    std::filesystem::path store_path_;
    std::filesystem::path history_path_;
    std::vector<ScheduledTask> scheduled_tasks_;
};

}  // namespace agentos
