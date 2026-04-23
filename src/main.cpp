#include "auth/auth_manager.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "auth/provider_adapters.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/orchestration/subagent_manager.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/codex_cli_agent.hpp"
#include "hosts/agents/mock_planning_agent.hpp"
#include "hosts/cli/cli_host.hpp"
#include "hosts/cli/cli_skill_invoker.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/http_fetch_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"
#include "trust/allowlist_store.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_manager.hpp"
#include "trust/trust_policy.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace agentos {

namespace {

struct Runtime {
    SkillRegistry skill_registry;
    AgentRegistry agent_registry;
    CliHost cli_host;
    SessionStore session_store;
    AuthProfileStore auth_profile_store;
    SecureTokenStore token_store;
    CredentialBroker credential_broker;
    AuthManager auth_manager;
    ExecutionCache execution_cache;
    Router router;
    IdentityManager identity_manager;
    AllowlistStore allowlist_store;
    PairingManager pairing_manager;
    TrustPolicy trust_policy;
    PolicyEngine policy_engine;
    MemoryManager memory_manager;
    Scheduler scheduler;
    AuditLogger audit_logger;
    SubagentManager subagent_manager;
    AgentLoop loop;

    explicit Runtime(const std::filesystem::path& workspace)
        : session_store(workspace / "runtime" / "auth_sessions.tsv"),
          auth_profile_store(workspace / "runtime" / "auth_profiles.tsv"),
          credential_broker(session_store, token_store),
          auth_manager(session_store, &auth_profile_store),
          execution_cache(workspace / "runtime" / "execution_cache.tsv"),
          identity_manager(workspace / "runtime" / "trust" / "identities.tsv"),
          allowlist_store(workspace / "runtime" / "trust" / "allowlist.tsv"),
          pairing_manager(allowlist_store),
          trust_policy(allowlist_store),
          policy_engine(trust_policy),
          memory_manager(workspace / "runtime" / "memory"),
          scheduler(workspace / "runtime" / "scheduler" / "tasks.tsv"),
          audit_logger(workspace / "runtime" / "audit.log"),
          subagent_manager(agent_registry, policy_engine, audit_logger, memory_manager),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {}
};

std::string MakeTaskId(const std::string& prefix) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return prefix + "-" + std::to_string(value);
}

TaskRequest BuildDemoWriteTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-write"),
        .task_type = "write_file",
        .objective = "Create a demo artifact for the minimal kernel.",
        .workspace_path = workspace,
        .inputs = {
            {"path", "runtime/demo_note.txt"},
            {"content", "AgentOS Phase 1 kernel scaffold\n"},
        },
    };
}

TaskRequest BuildDemoPatchTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-patch"),
        .task_type = "patch_file",
        .objective = "Patch the demo artifact to prove file mutation works.",
        .workspace_path = workspace,
        .inputs = {
            {"path", "runtime/demo_note.txt"},
            {"find", "Phase 1"},
            {"replace", "Phase 1 verified"},
        },
    };
}

TaskRequest BuildDemoReadTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-read"),
        .task_type = "read_file",
        .objective = "Read the demo artifact back through the skill registry.",
        .workspace_path = workspace,
        .inputs = {
            {"path", "runtime/demo_note.txt"},
        },
    };
}

TaskRequest BuildDemoAnalysisTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-analysis"),
        .task_type = "analysis",
        .objective = "Plan the next steps after the minimal kernel bootstrap.",
        .workspace_path = workspace,
        .preferred_target = std::string("mock_planner"),
    };
}

TaskRequest BuildDemoWorkflowTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-workflow"),
        .task_type = "workflow_run",
        .objective = "Run a composed workflow through the skill system.",
        .workspace_path = workspace,
        .inputs = {
            {"workflow", "write_patch_read"},
            {"path", "runtime/workflow_note.txt"},
            {"content", "AgentOS workflow kernel scaffold\n"},
            {"find", "workflow"},
            {"replace", "workflow verified"},
        },
    };
}

TaskRequest BuildDemoGitStatusTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-git-status"),
        .task_type = "git_status",
        .objective = "Inspect repository status through the CLI host.",
        .workspace_path = workspace,
    };
}

TaskRequest BuildDemoRgSearchTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-rg-search"),
        .task_type = "rg_search",
        .objective = "Search documentation through the CLI host.",
        .workspace_path = workspace,
        .inputs = {
            {"pattern", "AgentOS"},
            {"path", "README.md"},
        },
    };
}

TaskRequest BuildTaskFromArgs(const int argc, char* argv[], const std::filesystem::path& workspace) {
    TaskRequest task{
        .task_id = MakeTaskId("run"),
        .task_type = argv[2],
        .objective = std::string("Execute task type: ") + argv[2],
        .workspace_path = workspace,
    };

    for (int index = 3; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const auto key = argument.substr(0, separator);
        const auto value = argument.substr(separator + 1);

        if (key == "objective") {
            task.objective = value;
        } else if (key == "target") {
            task.preferred_target = value;
        } else if (key == "idempotency_key") {
            task.idempotency_key = value;
        } else if (key == "remote" || key == "remote_trigger") {
            task.remote_trigger = value == "true";
        } else if (key == "origin_identity" || key == "origin_identity_id") {
            task.origin_identity_id = value;
        } else if (key == "origin_device" || key == "origin_device_id") {
            task.origin_device_id = value;
        } else if (key == "allow_network") {
            task.allow_network = value == "true";
        } else if (key == "allow_high_risk") {
            task.allow_high_risk = value == "true";
        } else {
            task.inputs[key] = value;
        }
    }

    return task;
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

void PrintResult(const TaskRunResult& result) {
    std::cout << "success: " << (result.success ? "true" : "false") << '\n';
    std::cout << "from_cache: " << (result.from_cache ? "true" : "false") << '\n';
    std::cout << "route: " << route_target_kind_name(result.route_kind) << " -> " << result.route_target << '\n';
    std::cout << "summary: " << result.summary << '\n';

    if (!result.output_json.empty()) {
        std::cout << "output: " << result.output_json << '\n';
    }
    if (!result.error_code.empty()) {
        std::cout << "error_code: " << result.error_code << '\n';
    }
    if (!result.error_message.empty()) {
        std::cout << "error_message: " << result.error_message << '\n';
    }

    std::cout << '\n';
}

void PrintAuthSession(const AuthSession& session) {
    std::cout
        << ToString(session.provider)
        << " profile=" << session.profile_name
        << " mode=" << ToString(session.mode)
        << " account=" << session.account_label
        << " source=" << (session.managed_by_external_cli ? "external-cli" : "agentos")
        << '\n';
}

void PrintAuthStatus(const AuthStatus& status) {
    std::cout
        << status.provider_name
        << " profile=" << status.profile_name
        << " authenticated=" << (status.authenticated ? "true" : "false")
        << " expired=" << (status.expired ? "true" : "false")
        << " refreshable=" << (status.refreshable ? "true" : "false")
        << " mode=" << (status.mode.empty() ? "none" : status.mode)
        << " source=" << (status.managed_by_external_cli ? "external-cli" : (status.managed_by_agentos ? "agentos" : "none"))
        << " message=\"" << status.message << "\""
        << '\n';
}

void PrintAuthProviders(const AuthManager& auth_manager) {
    for (const auto& provider : auth_manager.providers()) {
        std::cout
            << provider.provider_name
            << " modes=";

        bool first = true;
        for (const auto mode : provider.supported_modes) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << ToString(mode);
        }

        std::cout
            << " browser=" << (provider.browser_login_supported ? "true" : "false")
            << " headless=" << (provider.headless_supported ? "true" : "false")
            << " cli_passthrough=" << (provider.cli_session_passthrough_supported ? "true" : "false")
            << '\n';
    }
}

void PrintSecureTokenStoreStatus(const SecureTokenStore& token_store) {
    const auto status = token_store.status();
    std::cout
        << "credential_store backend=" << status.backend_name
        << " system_keychain_backed=" << (status.system_keychain_backed ? "true" : "false")
        << " stores_plaintext=" << (status.stores_plaintext ? "true" : "false")
        << " dev_only=" << (status.dev_only ? "true" : "false")
        << " message=\"" << status.message << "\""
        << '\n';
}

void PrintAgents(const AgentRegistry& agent_registry) {
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto agent = agent_registry.find(profile.agent_name);
        std::cout
            << profile.agent_name
            << " healthy=" << (agent && agent->healthy() ? "true" : "false")
            << " session=" << (profile.supports_session ? "true" : "false")
            << " patch=" << (profile.supports_patch ? "true" : "false")
            << " risk=" << profile.risk_level
            << '\n';
    }
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
        "timeout_ms",
        "budget_limit",
    };

    return std::find(reserved.begin(), reserved.end(), key) != reserved.end();
}

bool IsReservedSubagentOption(const std::string& key) {
    static const std::vector<std::string> reserved{
        "agents",
        "mode",
        "task",
        "task_type",
        "objective",
        "id",
        "task_id",
        "idempotency_key",
        "remote",
        "remote_trigger",
        "origin_identity",
        "origin_identity_id",
        "origin_device",
        "origin_device_id",
        "allow_network",
        "allow_high_risk",
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
    const auto max_runs = options.contains("max_runs")
        ? ParseIntOption(options, "max_runs", interval_seconds > 0 ? 0 : 1)
        : (interval_seconds > 0 ? 0 : 1);

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
        .task = std::move(task),
    };
}

TaskRequest BuildSubagentTaskFromOptions(
    const std::map<std::string, std::string>& options,
    const std::filesystem::path& workspace) {
    const auto task_type = options.contains("task_type")
        ? options.at("task_type")
        : (options.contains("task") ? options.at("task") : "analysis");

    TaskRequest task{
        .task_id = options.contains("task_id") ? options.at("task_id") : MakeTaskId("subagents"),
        .task_type = task_type,
        .objective = options.contains("objective") ? options.at("objective") : ("Coordinate subagents for: " + task_type),
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
    };

    for (const auto& [key, value] : options) {
        if (!IsReservedSubagentOption(key)) {
            task.inputs[key] = value;
        }
    }

    return task;
}

void PrintTrustedPeer(const TrustedPeer& peer) {
    std::cout
        << peer.identity_id
        << " device=" << peer.device_id
        << " label=" << peer.label
        << " trust=" << ToString(peer.trust_level)
        << " permissions=";

    for (std::size_t index = 0; index < peer.permissions.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << peer.permissions[index];
    }
    std::cout << '\n';
}

void PrintIdentity(const Identity& identity) {
    std::cout
        << identity.identity_id
        << " user=" << identity.user_id
        << " label=" << identity.label
        << '\n';
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

void PrintMemorySummary(const MemoryManager& memory_manager) {
    std::cout
        << "tasks=" << memory_manager.task_log().size()
        << " skills=" << memory_manager.skill_stats().size()
        << " agents=" << memory_manager.agent_stats().size()
        << " workflow_candidates=" << memory_manager.workflow_candidates().size()
        << " workflows=" << memory_manager.workflow_store().list().size()
        << " lessons=" << memory_manager.lesson_store().list().size()
        << '\n';
}

void PrintMemoryStats(const MemoryManager& memory_manager) {
    for (const auto& [name, stats] : memory_manager.skill_stats()) {
        const auto success_rate = stats.total_calls == 0
            ? 0.0
            : static_cast<double>(stats.success_calls) / static_cast<double>(stats.total_calls);
        std::cout
            << "skill " << name
            << " total=" << stats.total_calls
            << " success=" << stats.success_calls
            << " success_rate=" << success_rate
            << " avg_latency_ms=" << stats.avg_latency_ms
            << '\n';
    }

    for (const auto& [name, stats] : memory_manager.agent_stats()) {
        const auto success_rate = stats.total_runs == 0
            ? 0.0
            : static_cast<double>(stats.success_runs) / static_cast<double>(stats.total_runs);
        std::cout
            << "agent " << name
            << " total=" << stats.total_runs
            << " success=" << stats.success_runs
            << " failed=" << stats.failed_runs
            << " success_rate=" << success_rate
            << " avg_duration_ms=" << stats.avg_duration_ms
            << '\n';
    }
}

void PrintWorkflowCandidates(const MemoryManager& memory_manager) {
    for (const auto& workflow : memory_manager.workflow_candidates()) {
        std::cout
            << workflow.name
            << " trigger=" << workflow.trigger_task_type
            << " score=" << workflow.score
            << " use_count=" << workflow.use_count
            << " success_count=" << workflow.success_count
            << " failure_count=" << workflow.failure_count
            << " success_rate=" << workflow.success_rate
            << " avg_duration_ms=" << workflow.avg_duration_ms
            << " steps=";

        for (std::size_t index = 0; index < workflow.ordered_steps.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << workflow.ordered_steps[index];
        }
        std::cout << '\n';
    }
}

void PrintWorkflowDefinition(const WorkflowDefinition& workflow) {
    std::cout
        << workflow.name
        << " enabled=" << (workflow.enabled ? "true" : "false")
        << " trigger=" << workflow.trigger_task_type
        << " source=" << workflow.source
        << " score=" << workflow.score
        << " use_count=" << workflow.use_count
        << " success_count=" << workflow.success_count
        << " failure_count=" << workflow.failure_count
        << " success_rate=" << workflow.success_rate
        << " avg_duration_ms=" << workflow.avg_duration_ms
        << " required_inputs=";

    for (std::size_t index = 0; index < workflow.required_inputs.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.required_inputs[index];
    }

    std::cout
        << " steps=";

    for (std::size_t index = 0; index < workflow.ordered_steps.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << workflow.ordered_steps[index];
    }
    std::cout << '\n';
}

void PrintWorkflowDefinitions(const WorkflowStore& workflow_store) {
    for (const auto& workflow : workflow_store.list()) {
        PrintWorkflowDefinition(workflow);
    }
}

void PrintLessons(const LessonStore& lesson_store) {
    for (const auto& lesson : lesson_store.list()) {
        std::cout
            << lesson.lesson_id
            << " enabled=" << (lesson.enabled ? "true" : "false")
            << " task_type=" << lesson.task_type
            << " target=" << lesson.target_name
            << " error_code=" << lesson.error_code
            << " occurrences=" << lesson.occurrence_count
            << " last_task_id=" << lesson.last_task_id
            << " summary=\"" << lesson.summary << "\""
            << '\n';
    }
}

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  agentos demo\n"
        << "  agentos cli-demo\n"
        << "  agentos agents\n"
        << "  agentos memory summary\n"
        << "  agentos memory stats\n"
        << "  agentos memory workflows\n"
        << "  agentos memory stored-workflows\n"
        << "  agentos memory lessons\n"
        << "  agentos memory promote-workflow <candidate_name> [workflow=<stored_name>] [required_inputs=a,b]\n"
        << "  agentos schedule add task=<task_type> due=now [recurrence=every:5m] [missed_run_policy=run-once|skip] key=value ...\n"
        << "  agentos schedule list\n"
        << "  agentos schedule history\n"
        << "  agentos schedule run-due\n"
        << "  agentos schedule tick [iterations=1] [interval_ms=1000]\n"
        << "  agentos schedule daemon [iterations=0] [interval_ms=1000]\n"
        << "  agentos schedule remove id=<schedule_id>\n"
        << "  agentos subagents run [agents=<agent[,agent]>] [mode=sequential|parallel] objective=text\n"
        << "  agentos trust identity-add identity=<id> [user=<user>] [label=name]\n"
        << "  agentos trust identities\n"
        << "  agentos trust identity-remove identity=<id>\n"
        << "  agentos trust pair identity=<id> device=<id> [label=name] [permissions=task.submit]\n"
        << "  agentos trust list\n"
        << "  agentos trust block identity=<id> device=<id>\n"
        << "  agentos trust remove identity=<id> device=<id>\n"
        << "  agentos auth providers\n"
        << "  agentos auth credential-store\n"
        << "  agentos auth status [provider] [profile=name]\n"
        << "  agentos auth login <provider> mode=api-key api_key_env=ENV_NAME [profile=name]\n"
        << "  agentos auth login <provider> mode=cli-session [profile=name]\n"
        << "  agentos auth default-profile <provider> profile=name\n"
        << "  agentos auth refresh <provider> [profile=name]\n"
        << "  agentos auth probe <provider>\n"
        << "  agentos run <task_type> key=value ...\n\n"
        << "Examples:\n"
        << "  agentos run read_file path=README.md\n"
        << "  agentos run write_file path=runtime/note.txt content=hello\n"
        << "  agentos run write_file path=runtime/note.txt content=hello idempotency_key=demo-write-1\n"
        << "  agentos memory promote-workflow write_file_workflow\n"
        << "  agentos schedule add id=demo-once task=write_file due=now path=runtime/scheduled.txt content=hello\n"
        << "  agentos schedule run-due\n"
        << "  agentos schedule tick iterations=1 interval_ms=0\n"
        << "  agentos schedule daemon iterations=1 interval_ms=0\n"
        << "  agentos subagents run agents=mock_planner mode=sequential objective=Plan_the_next_phase\n"
        << "  agentos trust identity-add identity=phone user=local-user label=dev-phone\n"
        << "  agentos trust pair identity=phone device=device1 label=dev-phone permissions=task.submit\n"
        << "  agentos run read_file path=README.md remote=true origin_identity=phone origin_device=device1\n"
        << "  agentos run workflow_run workflow=write_patch_read path=runtime/wf.txt content=hello find=hello replace=done\n"
        << "  agentos run rg_search pattern=AgentOS path=README.md\n"
        << "  agentos run http_fetch url=https://example.com allow_network=true\n"
        << "  agentos run analysis target=codex_cli objective=Review_the_project_structure\n"
        << "  agentos run analysis target=mock_planner objective=Design_the_next_phase\n";
}

int RunTrustCommand(
    IdentityManager& identity_manager,
    PairingManager& pairing_manager,
    AuditLogger& audit_logger,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    const auto command = std::string(argv[2]);
    const auto options = ParseOptionsFromArgs(argc, argv, 3);

    if (command == "identities") {
        for (const auto& identity : identity_manager.list()) {
            PrintIdentity(identity);
        }
        return 0;
    }

    if (command == "identity-add") {
        const auto identity_id = options.contains("identity") ? options.at("identity") : "";
        if (identity_id.empty()) {
            std::cerr << "identity is required\n";
            return 1;
        }

        PrintIdentity(identity_manager.save(Identity{
            .identity_id = identity_id,
            .user_id = options.contains("user") ? options.at("user") : "remote-user",
            .label = options.contains("label") ? options.at("label") : identity_id,
        }));
        audit_logger.record_trust_event("identity-add", identity_id, "", true, "identity saved");
        return 0;
    }

    if (command == "identity-remove") {
        const auto identity_id = options.contains("identity") ? options.at("identity") : "";
        if (identity_id.empty()) {
            std::cerr << "identity is required\n";
            return 1;
        }

        const auto removed = identity_manager.remove(identity_id);
        std::cout << (removed ? "removed " : "not_found ") << identity_id << '\n';
        audit_logger.record_trust_event(
            "identity-remove",
            identity_id,
            "",
            removed,
            removed ? "identity removed" : "identity not found");
        return removed ? 0 : 1;
    }

    if (command == "list") {
        for (const auto& peer : pairing_manager.list()) {
            PrintTrustedPeer(peer);
        }
        return 0;
    }

    const auto identity = options.contains("identity") ? options.at("identity") : "";
    const auto device = options.contains("device") ? options.at("device") : "";
    if (identity.empty() || device.empty()) {
        std::cerr << "identity and device are required\n";
        return 1;
    }

    if (command == "pair") {
        const auto label = options.contains("label") ? options.at("label") : identity + ":" + device;
        const bool identity_already_exists = identity_manager.find(identity).has_value();
        identity_manager.ensure(
            identity,
            options.contains("user") ? options.at("user") : "remote-user",
            options.contains("identity_label") ? options.at("identity_label") : identity);
        if (!identity_already_exists) {
            audit_logger.record_trust_event("identity-auto-create", identity, "", true, "identity created during pairing");
        }
        const auto permissions = options.contains("permissions")
            ? SplitCommaList(options.at("permissions"))
            : std::vector<std::string>{"task.submit"};
        PrintTrustedPeer(pairing_manager.pair(identity, device, label, permissions));
        audit_logger.record_trust_event("pair", identity, device, true, "peer paired");
        return 0;
    }

    if (command == "block") {
        pairing_manager.block(identity, device);
        std::cout << "blocked " << identity << " device=" << device << '\n';
        audit_logger.record_trust_event("block", identity, device, true, "peer blocked");
        return 0;
    }

    if (command == "remove") {
        pairing_manager.remove(identity, device);
        std::cout << "removed " << identity << " device=" << device << '\n';
        audit_logger.record_trust_event("remove", identity, device, true, "peer removed");
        return 0;
    }

    PrintUsage();
    return 1;
}

int RunScheduleCommand(
    Scheduler& scheduler,
    AgentLoop& loop,
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintUsage();
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
            std::cerr << "recurrence must use every:<n>s, every:<n>m, every:<n>h, or every:<n>d\n";
            return 1;
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

    PrintUsage();
    return 1;
}

int RunSubagentsCommand(
    SubagentManager& subagent_manager,
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    const auto command = std::string(argv[2]);
    const auto options = ParseOptionsFromArgs(argc, argv, 3);

    if (command == "run") {
        const auto agent_names = options.contains("agents")
            ? SplitCommaList(options.at("agents"))
            : std::vector<std::string>{};
        const auto mode = ParseSubagentExecutionMode(options.contains("mode") ? options.at("mode") : "sequential");
        const auto task = BuildSubagentTaskFromOptions(options, workspace);
        const auto result = subagent_manager.run(task, agent_names, mode);
        PrintResult(result);
        return result.success ? 0 : 1;
    }

    PrintUsage();
    return 1;
}

int RunMemoryCommand(MemoryManager& memory_manager, const int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[2];
    memory_manager.refresh_workflow_store();

    if (command == "summary") {
        PrintMemorySummary(memory_manager);
        return 0;
    }

    if (command == "stats") {
        PrintMemoryStats(memory_manager);
        return 0;
    }

    if (command == "workflows") {
        PrintWorkflowCandidates(memory_manager);
        return 0;
    }

    if (command == "stored-workflows") {
        PrintWorkflowDefinitions(memory_manager.workflow_store());
        return 0;
    }

    if (command == "lessons") {
        PrintLessons(memory_manager.lesson_store());
        return 0;
    }

    if (command == "promote-workflow") {
        const auto options = ParseOptionsFromArgs(argc, argv, 3);
        const auto candidate_name = options.contains("name")
            ? options.at("name")
            : (options.contains("candidate")
                ? options.at("candidate")
                : (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos ? std::string(argv[3]) : ""));
        if (candidate_name.empty()) {
            std::cerr << "workflow candidate name is required\n";
            return 1;
        }

        const auto candidates = memory_manager.workflow_candidates();
        const auto it = std::find_if(candidates.begin(), candidates.end(), [&](const WorkflowCandidate& candidate) {
            return candidate.name == candidate_name;
        });
        if (it == candidates.end()) {
            std::cerr << "workflow candidate not found: " << candidate_name << '\n';
            return 1;
        }

        auto workflow = WorkflowStore::FromCandidate(*it);
        workflow.source = "promoted_candidate";
        workflow.enabled = !options.contains("enabled") || options.at("enabled") != "false";
        if (options.contains("required_inputs")) {
            workflow.required_inputs = SplitCommaList(options.at("required_inputs"));
        }
        if (options.contains("workflow")) {
            workflow.name = options.at("workflow");
        } else if (options.contains("workflow_name")) {
            workflow.name = options.at("workflow_name");
        }

        PrintWorkflowDefinition(memory_manager.workflow_store().save(std::move(workflow)));
        return 0;
    }

    PrintUsage();
    return 1;
}

int RunAuthCommand(AuthManager& auth_manager, const SecureTokenStore& token_store, const int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[2];
    try {
        if (command == "providers") {
            PrintAuthProviders(auth_manager);
            return 0;
        }

        if (command == "credential-store") {
            PrintSecureTokenStoreStatus(token_store);
            return 0;
        }

        if (command == "status") {
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            if (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos) {
                const auto provider = ParseAuthProviderId(argv[3]);
                if (!provider.has_value()) {
                    std::cerr << "unknown provider: " << argv[3] << '\n';
                    return 1;
                }
                const auto profile = options.contains("profile")
                    ? options.at("profile")
                    : auth_manager.default_profile(*provider);
                PrintAuthStatus(auth_manager.status(*provider, profile));
            } else {
                for (const auto& descriptor : auth_manager.providers()) {
                    const auto profile = options.contains("profile")
                        ? options.at("profile")
                        : auth_manager.default_profile(descriptor.provider);
                    PrintAuthStatus(auth_manager.status(descriptor.provider, profile));
                }
            }
            return 0;
        }

        if (command == "login") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            if (!options.contains("profile")) {
                options["profile"] = auth_manager.default_profile(*provider);
            }

            const auto mode_text = options.contains("mode") ? options["mode"] : "api-key";
            const auto mode = ParseAuthMode(mode_text);
            if (!mode.has_value()) {
                std::cerr << "unknown auth mode: " << mode_text << '\n';
                return 1;
            }

            const auto session = auth_manager.login(*provider, *mode, options);
            PrintAuthSession(session);
            return 0;
        }

        if (command == "refresh") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);

            PrintAuthSession(auth_manager.refresh(*provider, profile));
            return 0;
        }

        if (command == "default-profile") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : (argc >= 5 && std::string(argv[4]).find('=') == std::string::npos ? std::string(argv[4]) : "");
            if (profile.empty()) {
                std::cerr << "profile is required\n";
                return 1;
            }

            auth_manager.set_default_profile(*provider, profile);
            std::cout << ToString(*provider) << " default_profile=" << profile << '\n';
            return 0;
        }

        if (command == "probe") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }

            const auto session = auth_manager.probe(*provider);
            if (!session.has_value()) {
                std::cout << ToString(*provider) << " external session: unavailable\n";
                return 1;
            }

            PrintAuthSession(*session);
            return 0;
        }

        if (command == "logout") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);

            auth_manager.logout(*provider, profile);
            std::cout << "logged out " << ToString(*provider) << " profile=" << profile << '\n';
            return 0;
        }
    } catch (const std::exception& error) {
        std::cerr << "auth error: " << error.what() << '\n';
        return 1;
    }

    PrintUsage();
    return 1;
}

}  // namespace

}  // namespace agentos

int main(int argc, char* argv[]) {
    using namespace agentos;

    const auto workspace = std::filesystem::current_path();
    Runtime runtime(workspace);

    runtime.skill_registry.register_skill(std::make_shared<FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<HttpFetchSkill>(runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeRgSearchSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeGitStatusSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeGitDiffSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeCurlFetchSpec(), runtime.cli_host));
    runtime.agent_registry.register_agent(std::make_shared<MockPlanningAgent>());
    runtime.agent_registry.register_agent(std::make_shared<CodexCliAgent>(runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<OpenAiAuthProviderAdapter>(
        runtime.session_store, runtime.token_store, runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<GeminiAuthProviderAdapter>(
        runtime.session_store, runtime.token_store));
    runtime.auth_manager.register_provider(std::make_shared<AnthropicAuthProviderAdapter>(
        runtime.session_store, runtime.token_store, runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<QwenAuthProviderAdapter>(
        runtime.session_store, runtime.token_store));

    if (argc == 1 || (argc >= 2 && std::string(argv[1]) == "demo")) {
        const auto write_result = runtime.loop.run(BuildDemoWriteTask(workspace));
        const auto patch_result = runtime.loop.run(BuildDemoPatchTask(workspace));
        const auto read_result = runtime.loop.run(BuildDemoReadTask(workspace));
        const auto workflow_result = runtime.loop.run(BuildDemoWorkflowTask(workspace));
        const auto analysis_result = runtime.loop.run(BuildDemoAnalysisTask(workspace));

        PrintResult(write_result);
        PrintResult(patch_result);
        PrintResult(read_result);
        PrintResult(workflow_result);
        PrintResult(analysis_result);

        std::cout << "audit_log: " << runtime.audit_logger.log_path().string() << '\n';
        std::cout << "task_log: " << (workspace / "runtime" / "memory" / "task_log.tsv").string() << '\n';
        std::cout << "workflow_candidates: " << runtime.memory_manager.workflow_candidates().size() << '\n';
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "cli-demo") {
        const auto git_status_result = runtime.loop.run(BuildDemoGitStatusTask(workspace));
        const auto rg_search_result = runtime.loop.run(BuildDemoRgSearchTask(workspace));

        PrintResult(git_status_result);
        PrintResult(rg_search_result);

        std::cout << "audit_log: " << runtime.audit_logger.log_path().string() << '\n';
        return git_status_result.success && rg_search_result.success ? 0 : 1;
    }

    if (argc >= 2 && std::string(argv[1]) == "agents") {
        PrintAgents(runtime.agent_registry);
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "auth") {
        return RunAuthCommand(runtime.auth_manager, runtime.token_store, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "memory") {
        return RunMemoryCommand(runtime.memory_manager, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "schedule") {
        return RunScheduleCommand(runtime.scheduler, runtime.loop, workspace, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "subagents") {
        return RunSubagentsCommand(runtime.subagent_manager, workspace, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "trust") {
        return RunTrustCommand(runtime.identity_manager, runtime.pairing_manager, runtime.audit_logger, argc, argv);
    }

    if (argc >= 3 && std::string(argv[1]) == "run") {
        const auto task = BuildTaskFromArgs(argc, argv, workspace);
        const auto result = runtime.loop.run(task);
        PrintResult(result);
        std::cout << "audit_log: " << runtime.audit_logger.log_path().string() << '\n';
        return result.success ? 0 : 1;
    }

    PrintUsage();
    return 1;
}
