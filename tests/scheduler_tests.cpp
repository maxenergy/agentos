#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/local_planning_agent.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <ctime>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_scheduler_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

long long LocalEpochMs(
    const int year,
    const int month,
    const int day,
    const int hour = 0,
    const int minute = 0) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = 0;
    value.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&value)) * 1000LL;
}

struct TestRuntime {
    agentos::SkillRegistry skill_registry;
    agentos::AgentRegistry agent_registry;
    agentos::Router router;
    agentos::PolicyEngine policy_engine;
    agentos::ExecutionCache execution_cache;
    agentos::AuditLogger audit_logger;
    agentos::MemoryManager memory_manager;
    agentos::AgentLoop loop;

    explicit TestRuntime(const std::filesystem::path& workspace)
        : execution_cache(workspace / "execution_cache.tsv"),
          audit_logger(workspace / "audit.log"),
          memory_manager(workspace / "memory"),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

void RegisterCore(TestRuntime& runtime) {
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<agentos::WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.agent_registry.register_agent(std::make_shared<agentos::LocalPlanningAgent>());
}

void TestSchedulerRunsDueTask(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(workspace / "scheduler" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "write-once",
        .enabled = true,
        .next_run_epoch_ms = agentos::Scheduler::NowEpochMs() - 1000,
        .interval_seconds = 0,
        .max_runs = 1,
        .run_count = 0,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "scheduled one-shot write",
            .workspace_path = workspace,
            .inputs = {
                {"path", "scheduled/once.txt"},
                {"content", "scheduled"},
            },
        },
    });

    const auto records = scheduler.run_due(runtime.loop, agentos::Scheduler::NowEpochMs());
    Expect(records.size() == 1, "scheduler should run one due task");
    Expect(records.front().result.success, "scheduled write_file should succeed");
    Expect(!records.front().rescheduled, "one-shot scheduled task should not be rescheduled");
    Expect(std::filesystem::exists(workspace / "scheduled" / "once.txt"), "scheduled write should create a file");

    const auto stored = scheduler.find("write-once");
    Expect(stored.has_value(), "scheduled task should remain inspectable after execution");
    if (stored.has_value()) {
        Expect(!stored->enabled, "one-shot scheduled task should be disabled after execution");
        Expect(stored->run_count == 1, "one-shot scheduled task should increment run_count");
    }

    agentos::Scheduler reloaded(workspace / "scheduler" / "tasks.tsv");
    const auto reloaded_task = reloaded.find("write-once");
    Expect(reloaded_task.has_value(), "scheduled task should reload from persisted store");
    if (reloaded_task.has_value()) {
        Expect(reloaded_task->run_count == 1, "scheduled task run_count should persist");
    }

    const auto history = reloaded.run_history();
    Expect(history.size() == 1, "scheduler execution history should persist one run record");
    if (!history.empty()) {
        Expect(history.front().schedule_id == "write-once", "scheduler history should preserve schedule id");
        Expect(history.front().task_id == "write-once.run-1", "scheduler history should preserve generated task id");
        Expect(history.front().success, "scheduler history should record success");
        Expect(history.front().route_target == "file_write", "scheduler history should record route target");
        Expect(history.front().run_count == 1, "scheduler history should record run count");
    }
    Expect(std::filesystem::exists(workspace / "scheduler" / "runs.tsv"), "scheduler runs.tsv should be written");
}

void TestSchedulerReschedulesIntervalTask(const std::filesystem::path& workspace) {
    TestRuntime runtime(workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(workspace / "scheduler_interval" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "write-twice",
        .enabled = true,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 60,
        .max_runs = 2,
        .run_count = 0,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "scheduled recurring write",
            .workspace_path = workspace,
            .inputs = {
                {"path", "scheduled/twice.txt"},
                {"content", "recurring"},
            },
        },
    });

    const auto first = scheduler.run_due(runtime.loop, 1000);
    Expect(first.size() == 1, "recurring scheduled task should run when due");
    Expect(first.front().rescheduled, "recurring scheduled task should reschedule before max_runs");

    const auto after_first = scheduler.find("write-twice");
    Expect(after_first.has_value(), "recurring scheduled task should remain after first run");
    if (after_first.has_value()) {
        Expect(after_first->enabled, "recurring scheduled task should remain enabled before max_runs");
        Expect(after_first->next_run_epoch_ms == 61000, "recurring scheduled task should advance next_run_epoch_ms");
    }

    const auto second = scheduler.run_due(runtime.loop, 61000);
    Expect(second.size() == 1, "recurring scheduled task should run a second time");
    Expect(!second.front().rescheduled, "recurring scheduled task should stop after max_runs");

    const auto after_second = scheduler.find("write-twice");
    Expect(after_second.has_value(), "recurring scheduled task should remain inspectable after max_runs");
    if (after_second.has_value()) {
        Expect(!after_second->enabled, "recurring scheduled task should disable after max_runs");
        Expect(after_second->run_count == 2, "recurring scheduled task should persist final run count");
    }
}

void TestSchedulerRetriesFailedTaskWithBackoff(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_retry_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(isolated_workspace / "scheduler_retry" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "retry-write",
        .enabled = true,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 0,
        .max_runs = 1,
        .run_count = 0,
        .max_retries = 1,
        .retry_count = 0,
        .retry_backoff_seconds = 60,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "scheduled retry write",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "scheduled/retry.txt"},
            },
        },
    });

    const auto first = scheduler.run_due(runtime.loop, 1000);
    Expect(first.size() == 1, "failed scheduled task should run once");
    Expect(!first.front().result.success, "scheduled task missing content should fail");
    Expect(first.front().rescheduled, "failed scheduled task should be rescheduled for retry");

    const auto after_first = scheduler.find("retry-write");
    Expect(after_first.has_value(), "retry scheduled task should remain after first failure");
    if (after_first.has_value()) {
        Expect(after_first->enabled, "retry scheduled task should remain enabled before retry is exhausted");
        Expect(after_first->run_count == 1, "retry scheduled task should count failed attempt");
        Expect(after_first->retry_count == 1, "retry scheduled task should increment retry count");
        Expect(after_first->next_run_epoch_ms == 61000, "retry scheduled task should apply backoff");
    }

    const auto second = scheduler.run_due(runtime.loop, 61000);
    Expect(second.size() == 1, "failed scheduled task should run retry");
    Expect(!second.front().result.success, "scheduled retry should still fail with missing content");
    Expect(!second.front().rescheduled, "failed scheduled task should stop after max_retries");

    const auto after_second = scheduler.find("retry-write");
    Expect(after_second.has_value(), "retry scheduled task should remain inspectable after retry exhaustion");
    if (after_second.has_value()) {
        Expect(!after_second->enabled, "retry scheduled task should disable after retry exhaustion");
        Expect(after_second->run_count == 2, "retry scheduled task should persist total attempts");
        Expect(after_second->retry_count == 1, "retry scheduled task should persist exhausted retry count");
    }

    agentos::Scheduler reloaded(isolated_workspace / "scheduler_retry" / "tasks.tsv");
    const auto reloaded_task = reloaded.find("retry-write");
    Expect(reloaded_task.has_value(), "retry fields should reload from persisted scheduler task");
    if (reloaded_task.has_value()) {
        Expect(reloaded_task->max_retries == 1, "max_retries should persist");
        Expect(reloaded_task->retry_backoff_seconds == 60, "retry_backoff_seconds should persist");
    }

    const auto history = reloaded.run_history();
    Expect(history.size() == 2, "scheduler retry history should record both attempts");
    if (history.size() == 2) {
        Expect(!history[0].success && history[0].rescheduled, "first retry history record should show reschedule");
        Expect(!history[1].success && !history[1].rescheduled, "second retry history record should show exhaustion");
    }
}

void TestSchedulerSkipsDisabledTask(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_disabled_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(isolated_workspace / "scheduler_disabled" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "disabled-write",
        .enabled = false,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 0,
        .max_runs = 1,
        .run_count = 0,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "disabled scheduled write",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "scheduled/disabled.txt"},
                {"content", "disabled"},
            },
        },
    });

    const auto records = scheduler.run_due(runtime.loop, 1000);
    Expect(records.empty(), "disabled scheduled task should not run when due");
    Expect(!std::filesystem::exists(isolated_workspace / "scheduled" / "disabled.txt"), "disabled scheduled task should not create a file");
    Expect(scheduler.run_history().empty(), "disabled scheduled task should not write run history");

    const auto stored = scheduler.find("disabled-write");
    Expect(stored.has_value(), "disabled scheduled task should remain persisted");
    if (stored.has_value()) {
        Expect(!stored->enabled, "disabled scheduled task should remain disabled");
        Expect(stored->run_count == 0, "disabled scheduled task should not increment run_count");
    }
}

void TestSchedulerMissedIntervalRunsOnceFromCurrentTime(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_missed_interval_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    agentos::Scheduler scheduler(isolated_workspace / "scheduler_missed" / "tasks.tsv");
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "missed-interval",
        .enabled = true,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 60,
        .max_runs = 0,
        .run_count = 0,
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "missed interval write",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "scheduled/missed.txt"},
                {"content", "missed"},
            },
        },
    });

    const auto records = scheduler.run_due(runtime.loop, 181000);
    Expect(records.size() == 1, "missed interval task should run only once per scheduler tick");
    if (records.size() == 1) {
        Expect(records.front().result.success, "missed interval task should succeed");
        Expect(records.front().rescheduled, "missed interval task should be rescheduled");
    }

    const auto stored = scheduler.find("missed-interval");
    Expect(stored.has_value(), "missed interval task should remain scheduled");
    if (stored.has_value()) {
        Expect(stored->run_count == 1, "missed interval task should record one run");
        Expect(stored->next_run_epoch_ms == 241000, "missed interval task should schedule next run from current scheduler time");
    }

    const auto history = scheduler.run_history();
    Expect(history.size() == 1, "missed interval task should write one scheduler history record");
    if (history.size() == 1) {
        Expect(history.front().success, "missed interval history should record success");
        Expect(history.front().rescheduled, "missed interval history should record reschedule");
        Expect(history.front().run_count == 1, "missed interval history should record one run");
    }
}

void TestSchedulerMissedIntervalSkipPolicy(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_missed_skip_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    const auto store_path = isolated_workspace / "scheduler_missed_skip" / "tasks.tsv";
    agentos::Scheduler scheduler(store_path);
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "skip-missed-interval",
        .enabled = true,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 60,
        .max_runs = 0,
        .run_count = 0,
        .missed_run_policy = "skip",
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "skip missed interval write",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "scheduled/skipped.txt"},
                {"content", "skipped"},
            },
        },
    });

    const auto records = scheduler.run_due(runtime.loop, 181000);
    Expect(records.empty(), "missed interval skip policy should not execute a stale run");
    Expect(!std::filesystem::exists(isolated_workspace / "scheduled" / "skipped.txt"), "missed interval skip policy should not create a file");
    Expect(scheduler.run_history().empty(), "missed interval skip policy should not write execution history");

    const auto stored = scheduler.find("skip-missed-interval");
    Expect(stored.has_value(), "missed interval skip policy task should remain scheduled");
    if (stored.has_value()) {
        Expect(stored->enabled, "missed interval skip policy task should remain enabled");
        Expect(stored->run_count == 0, "missed interval skip policy should not increment run_count");
        Expect(stored->next_run_epoch_ms == 241000, "missed interval skip policy should reschedule from current scheduler time");
        Expect(stored->missed_run_policy == "skip", "missed interval skip policy should remain configured");
    }

    agentos::Scheduler reloaded(store_path);
    const auto reloaded_task = reloaded.find("skip-missed-interval");
    Expect(reloaded_task.has_value(), "missed interval skip policy task should reload from persisted store");
    if (reloaded_task.has_value()) {
        Expect(reloaded_task->missed_run_policy == "skip", "missed interval skip policy should persist");
        Expect(reloaded_task->next_run_epoch_ms == 241000, "missed interval skip policy reschedule should persist");
    }
}

void TestSchedulerNormalizesInvalidMissedRunPolicy(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_invalid_policy_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    const auto store_path = isolated_workspace / "scheduler_invalid_policy" / "tasks.tsv";
    agentos::Scheduler scheduler(store_path);
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "invalid-policy",
        .enabled = true,
        .next_run_epoch_ms = 1000,
        .interval_seconds = 60,
        .max_runs = 0,
        .run_count = 0,
        .missed_run_policy = "later-maybe",
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "normalize invalid missed run policy",
            .workspace_path = isolated_workspace,
            .idempotency_key = "invalid-policy-write",
            .inputs = {
                {"path", "scheduled/normalized.txt"},
                {"content", "ok"},
            },
        },
    });

    const auto stored = scheduler.find("invalid-policy");
    Expect(stored.has_value(), "scheduler should persist task with invalid missed_run_policy input");
    if (stored.has_value()) {
        Expect(stored->missed_run_policy == "run-once",
            "scheduler should normalize invalid missed_run_policy to run-once");
    }

    agentos::Scheduler reloaded(store_path);
    const auto reloaded_task = reloaded.find("invalid-policy");
    Expect(reloaded_task.has_value(), "normalized missed_run_policy task should reload from persisted store");
    if (reloaded_task.has_value()) {
        Expect(reloaded_task->missed_run_policy == "run-once",
            "normalized missed_run_policy should persist as run-once");
    }
}

void TestSchedulerCronExpressionSupport(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_cron_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    Expect(agentos::Scheduler::IsCronExpressionValid("*/5 9-17 * * 1-5"),
        "scheduler should accept five-field cron expressions with steps and ranges");
    Expect(agentos::Scheduler::IsCronExpressionValid("@hourly"),
        "scheduler should accept @hourly cron alias");
    Expect(agentos::Scheduler::IsCronExpressionValid("@daily"),
        "scheduler should accept @daily cron alias");
    Expect(agentos::Scheduler::IsCronExpressionValid("@weekly"),
        "scheduler should accept @weekly cron alias");
    Expect(agentos::Scheduler::IsCronExpressionValid("@monthly"),
        "scheduler should accept @monthly cron alias");
    Expect(agentos::Scheduler::IsCronExpressionValid("@yearly"),
        "scheduler should accept @yearly cron alias");
    Expect(agentos::Scheduler::IsCronExpressionValid("@annually"),
        "scheduler should accept @annually cron alias");
    Expect(!agentos::Scheduler::IsCronExpressionValid("*/0 * * * *"),
        "scheduler should reject zero cron steps");
    const auto next_minute = agentos::Scheduler::NextCronRunEpochMs("* * * * *", 1000);
    Expect(next_minute.has_value() && *next_minute == 60000,
        "scheduler should compute the next cron minute after a timestamp");
    const auto next_hour = agentos::Scheduler::NextCronRunEpochMs("@hourly", 1000);
    Expect(next_hour.has_value() && *next_hour == 3600000,
        "scheduler should compute @hourly as the next top of hour");
    const auto next_yearly_alias = agentos::Scheduler::NextCronRunEpochMs("@yearly", 1000);
    const auto next_yearly_expression = agentos::Scheduler::NextCronRunEpochMs("0 0 1 1 *", 1000);
    Expect(next_yearly_alias.has_value() && next_yearly_expression.has_value() &&
            *next_yearly_alias == *next_yearly_expression,
        "@yearly should normalize to the Jan 1 midnight cron expression");
    // Cron evaluation now happens in UTC by default (was system-local before
    // wireup of the modular cron+tz). Use UTC instants directly so the test
    // is deterministic regardless of the machine's timezone.
    constexpr long long kJan2_1970_UtcMs = 86400000LL;       // 1970-01-02 00:00 UTC (Friday)
    constexpr long long kJan5_1970_UtcMs = 4LL * 86400000LL; // 1970-01-05 00:00 UTC (Monday)
    const auto next_dom_or_dow = agentos::Scheduler::NextCronRunEpochMs(
        "0 0 1 * 1", "UTC", kJan2_1970_UtcMs);
    Expect(next_dom_or_dow.has_value() && *next_dom_or_dow == kJan5_1970_UtcMs,
        "cron day-of-month and day-of-week restrictions should match with OR semantics");

    TestRuntime runtime(isolated_workspace);
    RegisterCore(runtime);

    const auto store_path = isolated_workspace / "scheduler_cron" / "tasks.tsv";
    agentos::Scheduler scheduler(store_path);
    scheduler.save(agentos::ScheduledTask{
        .schedule_id = "cron-write",
        .enabled = true,
        .next_run_epoch_ms = 60000,
        .interval_seconds = 0,
        .max_runs = 2,
        .run_count = 0,
        .cron_expression = "* * * * *",
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "cron scheduled write",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "scheduled/cron.txt"},
                {"content", "cron"},
            },
        },
    });

    const auto first = scheduler.run_due(runtime.loop, 60000);
    Expect(first.size() == 1, "cron scheduled task should run when due");
    Expect(first.front().result.success, "cron scheduled write should succeed");
    Expect(first.front().rescheduled, "cron scheduled task should reschedule before max_runs");

    const auto after_first = scheduler.find("cron-write");
    Expect(after_first.has_value(), "cron scheduled task should remain after first run");
    if (after_first.has_value()) {
        Expect(after_first->enabled, "cron scheduled task should remain enabled before max_runs");
        Expect(after_first->next_run_epoch_ms == 120000, "cron scheduled task should advance to the next matching minute");
        Expect(after_first->cron_expression == "* * * * *", "cron expression should remain configured");
    }

    agentos::Scheduler reloaded(store_path);
    const auto reloaded_task = reloaded.find("cron-write");
    Expect(reloaded_task.has_value(), "cron scheduled task should reload from persisted store");
    if (reloaded_task.has_value()) {
        Expect(reloaded_task->cron_expression == "* * * * *", "cron expression should persist");
        Expect(reloaded_task->next_run_epoch_ms == 120000, "cron next run should persist");
    }

    agentos::Scheduler alias_scheduler(isolated_workspace / "scheduler_cron_alias" / "tasks.tsv");
    alias_scheduler.save(agentos::ScheduledTask{
        .schedule_id = "cron-alias",
        .enabled = true,
        .next_run_epoch_ms = 3600000,
        .max_runs = 2,
        .cron_expression = "@hourly",
        .task = agentos::TaskRequest{
            .task_type = "write_file",
            .objective = "cron alias scheduled write",
            .workspace_path = isolated_workspace,
            .inputs = {
                {"path", "scheduled/cron-alias.txt"},
                {"content", "cron-alias"},
            },
        },
    });
    const auto alias_first = alias_scheduler.run_due(runtime.loop, 3600000);
    Expect(alias_first.size() == 1 && alias_first.front().result.success,
        "@hourly cron alias task should run when due");
    const auto alias_after_first = alias_scheduler.find("cron-alias");
    Expect(alias_after_first.has_value(), "@hourly cron alias task should remain inspectable");
    if (alias_after_first.has_value()) {
        Expect(alias_after_first->cron_expression == "@hourly", "cron alias should persist without normalization");
        Expect(alias_after_first->next_run_epoch_ms == 7200000, "@hourly cron alias should advance one hour");
    }
}

void TestSchedulerCronTimezoneAwareness(const std::filesystem::path& /*workspace*/) {
    Expect(agentos::Scheduler::IsTimezoneValid(""),
        "empty timezone should be accepted as UTC");
    Expect(agentos::Scheduler::IsTimezoneValid("UTC"),
        "literal UTC timezone should be accepted");
    Expect(agentos::Scheduler::IsTimezoneValid("UTC+08:00"),
        "fixed-offset timezone should be accepted");
    Expect(agentos::Scheduler::IsTimezoneValid("America/New_York"),
        "curated IANA zone should be accepted");
    Expect(!agentos::Scheduler::IsTimezoneValid("Mars/Olympus_Mons"),
        "unknown timezone should be rejected");

    // Cron 09:30 daily in Asia/Shanghai (UTC+8) should fire at 01:30 UTC.
    // Search starts from 1970-01-01 00:00 UTC; first match is 01:30 UTC same day.
    const auto next_shanghai = agentos::Scheduler::NextCronRunEpochMs(
        "30 9 * * *", "Asia/Shanghai", 0);
    constexpr long long kExpectShanghai = (1LL * 3600LL + 30LL * 60LL) * 1000LL;
    Expect(next_shanghai.has_value() && *next_shanghai == kExpectShanghai,
        "cron in Asia/Shanghai (UTC+8) should fire at 01:30 UTC for 09:30 local");

    // Same expression with explicit UTC+08:00 fixed offset should match.
    const auto next_offset = agentos::Scheduler::NextCronRunEpochMs(
        "30 9 * * *", "UTC+08:00", 0);
    Expect(next_offset.has_value() && *next_offset == kExpectShanghai,
        "fixed-offset UTC+08:00 should match Asia/Shanghai for non-DST zones");

    // Cron 09:30 daily in UTC (default behavior) should fire at 09:30 UTC.
    const auto next_utc = agentos::Scheduler::NextCronRunEpochMs(
        "30 9 * * *", "UTC", 0);
    constexpr long long kExpectUtc = (9LL * 3600LL + 30LL * 60LL) * 1000LL;
    Expect(next_utc.has_value() && *next_utc == kExpectUtc,
        "cron in UTC should fire at 09:30 UTC");
}

void TestSchedulerCronTimezoneRoundTrip(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_cron_tz_roundtrip";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    const auto store_path = isolated_workspace / "tasks.tsv";
    {
        agentos::Scheduler scheduler(store_path);
        scheduler.save(agentos::ScheduledTask{
            .schedule_id = "cron-tz",
            .enabled = true,
            .next_run_epoch_ms = 0,
            .max_runs = 0,
            .cron_expression = "30 9 * * *",
            .timezone_name = "Asia/Shanghai",
            .task = agentos::TaskRequest{
                .task_type = "write_file",
                .objective = "tz cron",
                .workspace_path = isolated_workspace,
                .inputs = {{"path", "tz.txt"}, {"content", "tz"}},
            },
        });
    }

    agentos::Scheduler reloaded(store_path);
    const auto roundtrip = reloaded.find("cron-tz");
    Expect(roundtrip.has_value(), "tz scheduled task should reload from TSV");
    if (roundtrip.has_value()) {
        Expect(roundtrip->cron_expression == "30 9 * * *",
            "cron expression should persist across reload");
        Expect(roundtrip->timezone_name == "Asia/Shanghai",
            "timezone_name should persist across reload");
    }
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestSchedulerRunsDueTask(workspace);
    TestSchedulerReschedulesIntervalTask(workspace);
    TestSchedulerRetriesFailedTaskWithBackoff(workspace);
    TestSchedulerSkipsDisabledTask(workspace);
    TestSchedulerMissedIntervalRunsOnceFromCurrentTime(workspace);
    TestSchedulerMissedIntervalSkipPolicy(workspace);
    TestSchedulerNormalizesInvalidMissedRunPolicy(workspace);
    TestSchedulerCronExpressionSupport(workspace);
    TestSchedulerCronTimezoneAwareness(workspace);
    TestSchedulerCronTimezoneRoundTrip(workspace);

    if (failures != 0) {
        std::cerr << failures << " scheduler test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_scheduler_tests passed\n";
    return 0;
}
