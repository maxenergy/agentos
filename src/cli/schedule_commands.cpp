#include "cli/schedule_commands.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentos {

namespace {

std::string MakeTaskId(const std::string& prefix) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return prefix + "-" + std::to_string(value);
}

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument.rfind("--", 0) == 0) {
            argument = argument.substr(2);
        }

        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        options[argument.substr(0, separator)] = argument.substr(separator + 1);
    }
    return options;
}

std::vector<std::string> SplitCommaList(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

bool ParseBoolOption(const std::map<std::string, std::string>& options, const std::string& key, const bool fallback = false) {
    if (!options.contains(key)) {
        return fallback;
    }

    const auto value = options.at(key);
    return value == "true" || value == "1" || value == "yes";
}

int ParseIntOption(const std::map<std::string, std::string>& options, const std::string& key, const int fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stoi(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

long long ParseLongLongOption(
    const std::map<std::string, std::string>& options,
    const std::string& key,
    const long long fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stoll(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

double ParseDoubleOption(const std::map<std::string, std::string>& options, const std::string& key, const double fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stod(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

bool IsValidMissedRunPolicy(const std::string& value) {
    return value == "run-once" || value == "skip";
}

int ParseRecurrenceIntervalSeconds(const std::map<std::string, std::string>& options) {
    if (options.contains("interval_seconds")) {
        return ParseIntOption(options, "interval_seconds", 0);
    }
    if (!options.contains("recurrence")) {
        return 0;
    }

    const auto recurrence = options.at("recurrence");
    if (recurrence.rfind("cron:", 0) == 0) {
        return 0;
    }
    constexpr std::string_view prefix = "every:";
    if (recurrence.rfind(std::string(prefix), 0) != 0 || recurrence.size() <= prefix.size() + 1) {
        return -1;
    }

    const auto suffix = recurrence.back();
    const auto value_text = recurrence.substr(prefix.size(), recurrence.size() - prefix.size() - 1);
    int value = 0;
    try {
        value = std::stoi(value_text);
    } catch (const std::exception&) {
        return -1;
    }
    if (value <= 0) {
        return -1;
    }

    switch (suffix) {
    case 's':
        return value;
    case 'm':
        return value * 60;
    case 'h':
        return value * 60 * 60;
    case 'd':
        return value * 24 * 60 * 60;
    default:
        return -1;
    }
}

std::string ParseCronExpression(const std::map<std::string, std::string>& options) {
    if (options.contains("cron")) {
        return options.at("cron");
    }
    if (!options.contains("recurrence")) {
        return "";
    }

    const auto recurrence = options.at("recurrence");
    constexpr std::string_view prefix = "cron:";
    if (recurrence.rfind(std::string(prefix), 0) != 0) {
        return "";
    }
    return recurrence.substr(prefix.size());
}

long long ParseDueEpochMs(const std::map<std::string, std::string>& options) {
    const auto now = Scheduler::NowEpochMs();
    if (options.contains("delay_seconds")) {
        return now + (ParseLongLongOption(options, "delay_seconds", 0) * 1000LL);
    }

    if (!options.contains("due") || options.at("due") == "now") {
        return now;
    }

    return ParseLongLongOption(options, "due", now);
}

bool IsReservedScheduleOption(const std::string& key) {
    static const std::vector<std::string> reserved{
        "id",
        "schedule_id",
        "task",
        "task_type",
        "due",
        "delay_seconds",
        "recurrence",
        "cron",
        "timezone",
        "tz",
        "interval_seconds",
        "max_runs",
        "max_retries",
        "retry_backoff_seconds",
        "missed_run_policy",
        "objective",
        "target",
        "idempotency_key",
        "remote",
        "remote_trigger",
        "origin_identity",
        "origin_identity_id",
        "origin_device",
        "origin_device_id",
        "allow_network",
        "allow_high_risk",
        "approval_id",
        "permission_grants",
        "grants",
        "timeout_ms",
        "budget_limit",
    };

    return std::find(reserved.begin(), reserved.end(), key) != reserved.end();
}

ScheduledTask BuildScheduledTaskFromOptions(
    const std::map<std::string, std::string>& options,
    const std::filesystem::path& workspace) {
    const auto schedule_id = options.contains("schedule_id")
        ? options.at("schedule_id")
        : (options.contains("id") ? options.at("id") : MakeTaskId("schedule"));
    const auto task_type = options.contains("task_type")
        ? options.at("task_type")
        : (options.contains("task") ? options.at("task") : "");
    const auto interval_seconds = ParseRecurrenceIntervalSeconds(options);
    const auto cron_expression = ParseCronExpression(options);
    const auto max_runs = options.contains("max_runs")
        ? ParseIntOption(options, "max_runs", (interval_seconds > 0 || !cron_expression.empty()) ? 0 : 1)
        : ((interval_seconds > 0 || !cron_expression.empty()) ? 0 : 1);

    TaskRequest task{
        .task_id = "",
        .task_type = task_type,
        .objective = options.contains("objective") ? options.at("objective") : ("Scheduled task: " + task_type),
        .workspace_path = workspace,
        .idempotency_key = options.contains("idempotency_key") ? options.at("idempotency_key") : "",
        .remote_trigger = ParseBoolOption(options, "remote", ParseBoolOption(options, "remote_trigger", false)),
        .origin_identity_id = options.contains("origin_identity")
            ? options.at("origin_identity")
            : (options.contains("origin_identity_id") ? options.at("origin_identity_id") : ""),
        .origin_device_id = options.contains("origin_device")
            ? options.at("origin_device")
            : (options.contains("origin_device_id") ? options.at("origin_device_id") : ""),
        .timeout_ms = ParseIntOption(options, "timeout_ms", 5000),
        .budget_limit = ParseDoubleOption(options, "budget_limit", 0.0),
        .allow_high_risk = ParseBoolOption(options, "allow_high_risk", false),
        .allow_network = ParseBoolOption(options, "allow_network", false),
        .approval_id = options.contains("approval_id") ? options.at("approval_id") : "",
        .permission_grants = options.contains("permission_grants")
            ? SplitCommaList(options.at("permission_grants"))
            : (options.contains("grants") ? SplitCommaList(options.at("grants")) : std::vector<std::string>{}),
    };

    if (options.contains("target")) {
        task.preferred_target = options.at("target");
    }

    for (const auto& [key, value] : options) {
        if (!IsReservedScheduleOption(key)) {
            task.inputs[key] = value;
        }
    }

    return ScheduledTask{
        .schedule_id = schedule_id,
        .enabled = true,
        .next_run_epoch_ms = ParseDueEpochMs(options),
        .interval_seconds = interval_seconds,
        .max_runs = max_runs,
        .run_count = 0,
        .max_retries = ParseIntOption(options, "max_retries", 0),
        .retry_count = 0,
        .retry_backoff_seconds = ParseIntOption(options, "retry_backoff_seconds", 0),
        .missed_run_policy = options.contains("missed_run_policy") ? options.at("missed_run_policy") : "run-once",
        .cron_expression = cron_expression,
        .timezone_name = options.contains("timezone")
            ? options.at("timezone")
            : (options.contains("tz") ? options.at("tz") : ""),
        .task = std::move(task),
    };
}

void PrintScheduleUsage() {
    std::cerr
        << "schedule commands:\n"
        << "  agentos schedule add task=<task_type> due=now [recurrence=every:5m|cron:<expr>] [cron=\"*/5 * * * *\"|@hourly|@daily|@weekly|@monthly|@yearly] [timezone=America/New_York|UTC+08:00] [missed_run_policy=run-once|skip] key=value ...\n"
        << "  agentos schedule list\n"
        << "  agentos schedule history\n"
        << "  agentos schedule run-due\n"
        << "  agentos schedule tick [iterations=1] [interval_ms=1000]\n"
        << "  agentos schedule daemon [iterations=0] [interval_ms=1000]\n";
}

void PrintScheduledTask(const ScheduledTask& task) {
    std::cout
        << task.schedule_id
        << " enabled=" << (task.enabled ? "true" : "false")
        << " next_run_epoch_ms=" << task.next_run_epoch_ms
        << " interval_seconds=" << task.interval_seconds
        << " max_runs=" << task.max_runs
        << " run_count=" << task.run_count
        << " max_retries=" << task.max_retries
        << " retry_count=" << task.retry_count
        << " retry_backoff_seconds=" << task.retry_backoff_seconds
        << " missed_run_policy=" << task.missed_run_policy
        << " cron=\"" << task.cron_expression << "\""
        << " timezone=\"" << task.timezone_name << "\""
        << " task_type=" << task.task.task_type
        << " objective=\"" << task.task.objective << "\""
        << '\n';
}

bool PrintSchedulerRunRecords(const std::vector<SchedulerRunRecord>& records) {
    if (records.empty()) {
        std::cout << "no due scheduled tasks\n";
        return true;
    }

    bool all_success = true;
    for (const auto& record : records) {
        all_success = all_success && record.result.success;
        std::cout
            << record.schedule_id
            << " success=" << (record.result.success ? "true" : "false")
            << " rescheduled=" << (record.rescheduled ? "true" : "false")
            << " route=" << route_target_kind_name(record.result.route_kind) << "->" << record.result.route_target
            << " summary=\"" << record.result.summary << "\"";
        if (!record.result.error_code.empty()) {
            std::cout << " error_code=" << record.result.error_code;
        }
        std::cout << '\n';
    }
    return all_success;
}

bool RunSchedulerLoop(
    Scheduler& scheduler,
    AgentLoop& loop,
    const std::string& label,
    const int iterations,
    const int interval_ms) {
    bool all_success = true;
    int iteration = 0;
    while (iterations == 0 || iteration < iterations) {
        ++iteration;
        const auto now = Scheduler::NowEpochMs();
        std::cout
            << label << " iteration=" << iteration
            << " now_epoch_ms=" << now
            << '\n';
        all_success = PrintSchedulerRunRecords(scheduler.run_due(loop, now)) && all_success;

        if (iterations != 0 && iteration >= iterations) {
            break;
        }
        if (interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }
    return all_success;
}

void PrintSchedulerHistory(const std::vector<SchedulerExecutionRecord>& records) {
    for (const auto& record : records) {
        std::cout
            << record.schedule_id
            << " task_id=" << record.task_id
            << " run_count=" << record.run_count
            << " success=" << (record.success ? "true" : "false")
            << " rescheduled=" << (record.rescheduled ? "true" : "false")
            << " route=" << record.route_kind << "->" << record.route_target
            << " duration_ms=" << record.duration_ms;
        if (!record.error_code.empty()) {
            std::cout << " error_code=" << record.error_code;
        }
        std::cout << '\n';
    }
}

}  // namespace

int RunScheduleCommand(
    Scheduler& scheduler,
    AgentLoop& loop,
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintScheduleUsage();
        return 1;
    }

    const auto command = std::string(argv[2]);
    const auto options = ParseOptionsFromArgs(argc, argv, 3);

    if (command == "list") {
        for (const auto& task : scheduler.list()) {
            PrintScheduledTask(task);
        }
        return 0;
    }

    if (command == "history") {
        PrintSchedulerHistory(scheduler.run_history());
        return 0;
    }

    if (command == "add") {
        auto scheduled_task = BuildScheduledTaskFromOptions(options, workspace);
        if (scheduled_task.schedule_id.empty() || scheduled_task.task.task_type.empty()) {
            std::cerr << "schedule id and task/task_type are required\n";
            return 1;
        }
        if (scheduled_task.interval_seconds < 0) {
            std::cerr << "recurrence must use every:<n>s, every:<n>m, every:<n>h, every:<n>d, or cron:<expr>\n";
            return 1;
        }
        if (scheduled_task.interval_seconds > 0 && !scheduled_task.cron_expression.empty()) {
            std::cerr << "recurrence interval and cron expression are mutually exclusive\n";
            return 1;
        }
        if (!scheduled_task.cron_expression.empty() &&
            !Scheduler::IsCronExpressionValid(scheduled_task.cron_expression)) {
            std::cerr << "cron expression must use five fields: minute hour day-of-month month day-of-week, or @hourly/@daily/@weekly/@monthly/@yearly/@annually\n";
            return 1;
        }
        if (!Scheduler::IsTimezoneValid(scheduled_task.timezone_name)) {
            std::cerr << "timezone must be UTC, a fixed offset (UTC+HH:MM / UTC-HH:MM), or a recognized IANA zone\n";
            return 1;
        }
        if (!scheduled_task.cron_expression.empty() && scheduled_task.next_run_epoch_ms <= Scheduler::NowEpochMs()) {
            scheduled_task.next_run_epoch_ms = Scheduler::NextCronRunEpochMs(
                scheduled_task.cron_expression, scheduled_task.timezone_name,
                Scheduler::NowEpochMs()).value_or(scheduled_task.next_run_epoch_ms);
        }
        if (scheduled_task.max_retries < 0 || scheduled_task.retry_backoff_seconds < 0) {
            std::cerr << "max_retries and retry_backoff_seconds must be non-negative\n";
            return 1;
        }
        if (!IsValidMissedRunPolicy(scheduled_task.missed_run_policy)) {
            std::cerr << "missed_run_policy must be run-once or skip\n";
            return 1;
        }

        PrintScheduledTask(scheduler.save(std::move(scheduled_task)));
        return 0;
    }

    if (command == "remove") {
        const auto id = options.contains("id")
            ? options.at("id")
            : (options.contains("schedule_id") ? options.at("schedule_id") : "");
        if (id.empty()) {
            std::cerr << "id is required\n";
            return 1;
        }

        const auto removed = scheduler.remove(id);
        std::cout << (removed ? "removed " : "not_found ") << id << '\n';
        return removed ? 0 : 1;
    }

    if (command == "run-due") {
        const auto now = ParseLongLongOption(options, "now_epoch_ms", Scheduler::NowEpochMs());
        const auto records = scheduler.run_due(loop, now);
        return PrintSchedulerRunRecords(records) ? 0 : 1;
    }

    if (command == "tick") {
        const auto iterations = ParseIntOption(options, "iterations", 1);
        const auto interval_ms = ParseIntOption(options, "interval_ms", 1000);
        if (iterations < 0 || interval_ms < 0) {
            std::cerr << "iterations and interval_ms must be non-negative\n";
            return 1;
        }

        return RunSchedulerLoop(scheduler, loop, "tick", iterations, interval_ms) ? 0 : 1;
    }

    if (command == "daemon") {
        const auto iterations = ParseIntOption(options, "iterations", 0);
        const auto interval_ms = ParseIntOption(options, "interval_ms", 1000);
        if (iterations < 0 || interval_ms < 0) {
            std::cerr << "iterations and interval_ms must be non-negative\n";
            return 1;
        }

        std::cout
            << "daemon mode=foreground"
            << " iterations=" << iterations
            << " interval_ms=" << interval_ms
            << '\n';
        return RunSchedulerLoop(scheduler, loop, "daemon", iterations, interval_ms) ? 0 : 1;
    }

    PrintScheduleUsage();
    return 1;
}

}  // namespace agentos
