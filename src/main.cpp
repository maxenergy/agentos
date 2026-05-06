#include "auth/auth_manager.hpp"
#include "auth/auth_profile_store.hpp"
#include "auth/credential_broker.hpp"
#include "auth/provider_adapters.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "cli/agents_commands.hpp"
#include "cli/autodev_commands.hpp"
#include "cli/auth_commands.hpp"
#include "cli/cli_specs_commands.hpp"
#include "cli/diagnostics_commands.hpp"
#include "cli/interactive_commands.hpp"
#include "cli/main_agent_commands.hpp"
#include "cli/serve_commands.hpp"
#include "cli/memory_commands.hpp"
#include "cli/plugins_commands.hpp"
#include "cli/schedule_commands.hpp"
#include "cli/storage_commands.hpp"
#include "cli/subagents_commands.hpp"
#include "cli/trust_commands.hpp"
#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/orchestration/subagent_manager.hpp"
#include "core/policy/approval_store.hpp"
#include "core/policy/policy_engine.hpp"
#include "core/policy/role_catalog.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/router/router.hpp"
#include "hosts/agents/anthropic_agent.hpp"
#include "hosts/agents/codex_cli_agent.hpp"
#include "hosts/agents/gemini_agent.hpp"
#include "hosts/agents/local_planning_agent.hpp"
#include "hosts/agents/main_agent.hpp"
#include "hosts/agents/openai_agent.hpp"
#include "hosts/agents/qwen_agent.hpp"
#include "hosts/cli/cli_host.hpp"
#include "hosts/cli/cli_skill_invoker.hpp"
#include "hosts/cli/cli_spec_loader.hpp"
#include "hosts/plugin/plugin_host.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "skills/builtin/development_skill.hpp"
#include "skills/builtin/file_patch_skill.hpp"
#include "skills/builtin/file_read_skill.hpp"
#include "skills/builtin/file_write_skill.hpp"
#include "skills/builtin/host_info_skill.hpp"
#include "skills/builtin/http_fetch_skill.hpp"
#include "skills/builtin/learn_skill.hpp"
#include "skills/builtin/news_search_skill.hpp"
#include "skills/builtin/research_skill.hpp"
#include "skills/builtin/workflow_run_skill.hpp"
#include "storage/main_agent_store.hpp"
#include "storage/storage_version_store.hpp"
#include "storage/storage_export.hpp"
#include "storage/storage_policy.hpp"
#include "storage/storage_transaction.hpp"
#include "trust/allowlist_store.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_invite_store.hpp"
#include "trust/pairing_manager.hpp"
#include "trust/trust_policy.hpp"
#include "utils/command_utils.hpp"
#include "utils/signal_cancellation.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
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
    PluginHost plugin_host;
    SessionStore session_store;
    AuthProfileStore auth_profile_store;
    SecureTokenStore token_store;
    CredentialBroker credential_broker;
    AuthManager auth_manager;
    MainAgentStore main_agent_store;
    ExecutionCache execution_cache;
    Router router;
    IdentityManager identity_manager;
    AllowlistStore allowlist_store;
    PairingInviteStore pairing_invite_store;
    PairingManager pairing_manager;
    TrustPolicy trust_policy;
    RoleCatalog role_catalog;
    ApprovalStore approval_store;
    PolicyEngine policy_engine;
    MemoryManager memory_manager;
    StorageVersionStore storage_version_store;
    Scheduler scheduler;
    AuditLogger audit_logger;
    SubagentManager subagent_manager;
    AgentLoop loop;

    explicit Runtime(const std::filesystem::path& workspace)
        : session_store(workspace / "runtime" / "auth_sessions.tsv"),
          auth_profile_store(workspace / "runtime" / "auth_profiles.tsv"),
          plugin_host(cli_host, LoadPluginHostOptions(workspace)),
          credential_broker(session_store, token_store),
          auth_manager(session_store, &auth_profile_store),
          main_agent_store(workspace / "runtime" / "main_agent.tsv"),
          execution_cache(workspace / "runtime" / "execution_cache.tsv"),
          identity_manager(workspace / "runtime" / "trust" / "identities.tsv"),
          allowlist_store(workspace / "runtime" / "trust" / "allowlist.tsv"),
          pairing_invite_store(workspace / "runtime" / "trust" / "invites.tsv"),
          pairing_manager(allowlist_store),
          trust_policy(allowlist_store),
          role_catalog(workspace / "runtime" / "trust" / "roles.tsv"),
          approval_store(workspace / "runtime" / "trust" / "approvals.tsv"),
          policy_engine(PolicyEngineDependencies{
              .trust_policy = &trust_policy,
              .role_catalog = &role_catalog,
              .approval_store = &approval_store,
          }),
          memory_manager(workspace / "runtime" / "memory"),
          storage_version_store(workspace / "runtime" / "storage_manifest.tsv"),
          scheduler(workspace / "runtime" / "scheduler" / "tasks.tsv"),
          audit_logger(workspace / "runtime" / "audit.log"),
          subagent_manager(agent_registry, policy_engine, audit_logger, memory_manager),
          loop(skill_registry, agent_registry, router, policy_engine, audit_logger, memory_manager, execution_cache) {
        storage_version_store.ensure_current();
    }
};

std::string MakeTaskId(const std::string& prefix) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return prefix + "-" + std::to_string(value);
}

std::vector<std::string> SplitCommaList(const std::string& value);
int ParseIntOption(const std::map<std::string, std::string>& options, const std::string& key, int fallback);
double ParseDoubleOption(const std::map<std::string, std::string>& options, const std::string& key, double fallback);

TaskRequest BuildDemoWriteTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-write"),
        .task_type = "write_file",
        .objective = "Create a demo artifact for the minimal kernel.",
        .workspace_path = workspace,
        .idempotency_key = "demo-write",
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
        .idempotency_key = "demo-patch",
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
        .preferred_target = std::string("local_planner"),
    };
}

TaskRequest BuildDemoWorkflowTask(const std::filesystem::path& workspace) {
    return {
        .task_id = MakeTaskId("demo-workflow"),
        .task_type = "workflow_run",
        .objective = "Run a composed workflow through the skill system.",
        .workspace_path = workspace,
        .idempotency_key = "demo-workflow",
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
        } else if (key == "user" || key == "user_id") {
            task.user_id = value;
        } else if (key == "remote" || key == "remote_trigger") {
            task.remote_trigger = value == "true";
        } else if (key == "origin_identity" || key == "origin_identity_id") {
            task.origin_identity_id = value;
        } else if (key == "origin_device" || key == "origin_device_id") {
            task.origin_device_id = value;
        } else if (key == "profile" || key == "auth_profile") {
            task.auth_profile = value;
        } else if (key == "allow_network") {
            task.allow_network = value == "true";
        } else if (key == "allow_high_risk") {
            task.allow_high_risk = value == "true";
        } else if (key == "approval_id") {
            task.approval_id = value;
        } else if (key == "permission_grants" || key == "grants") {
            task.permission_grants = SplitCommaList(value);
        } else if (key == "timeout_ms") {
            task.timeout_ms = ParseIntOption({{key, value}}, key, task.timeout_ms);
        } else if (key == "budget_limit") {
            task.budget_limit = ParseDoubleOption({{key, value}}, key, task.budget_limit);
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
        .auth_profile = options.contains("profile")
            ? std::make_optional(options.at("profile"))
            : (options.contains("auth_profile") ? std::make_optional(options.at("auth_profile")) : std::nullopt),
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
        .task = std::move(task),
    };
}

std::set<std::string> BuiltinSkillNames() {
    return {
        "file_read",
        "file_write",
        "file_patch",
        "http_fetch",
        "host_info",
        "workflow_run",
        "rg_search",
        "git_status",
        "git_diff",
        "curl_fetch",
        "jq_transform",
    };
}

std::string CliSpecConflictReason(const std::string& name) {
    return "external CLI spec name conflicts with already registered skill: " + name;
}

void RegisterExternalCliSpecs(Runtime& runtime, const std::filesystem::path& workspace) {
    const auto loaded = LoadCliSpecsWithDiagnostics(workspace / "runtime" / "cli_specs");
    for (const auto& spec : loaded.specs) {
        if (runtime.skill_registry.find(spec.name)) {
            runtime.audit_logger.record_config_diagnostic(
                "cli_spec",
                spec.source_file,
                spec.source_line_number,
                CliSpecConflictReason(spec.name));
            continue;
        }
        if (!CommandExists(spec.binary)) {
            runtime.audit_logger.record_config_diagnostic(
                "cli_spec",
                spec.source_file,
                spec.source_line_number,
                "CLI spec binary is not available on this host: " + spec.binary);
            continue;
        }
        runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(spec, runtime.cli_host));
    }
    for (const auto& diagnostic : loaded.diagnostics) {
        runtime.audit_logger.record_config_diagnostic(
            "cli_spec", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
}

void RegisterPluginSpecs(Runtime& runtime, const std::filesystem::path& workspace) {
    const auto loaded = LoadPluginSpecsWithDiagnostics(workspace / "runtime" / "plugin_specs");
    for (const auto& spec : loaded.specs) {
        if (runtime.skill_registry.find(spec.name)) {
            runtime.audit_logger.record_config_diagnostic(
                "plugin_spec",
                spec.source_file,
                spec.source_line_number,
                PluginSpecConflictReason(spec.name));
            continue;
        }
        runtime.skill_registry.register_skill(std::make_shared<PluginSkillInvoker>(spec, runtime.plugin_host));
    }
    for (const auto& diagnostic : loaded.diagnostics) {
        runtime.audit_logger.record_config_diagnostic(
            "plugin_spec", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
}

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  agentos demo\n"
        << "  agentos cli-demo\n"
        << "  agentos interactive\n"
        << "  agentos serve [port=18080] [host=127.0.0.1]\n"
        << "  agentos agents\n"
        << "  agentos autodev submit target_repo_path=<path> objective=<text> [skill_pack_path=<path>] [isolation_mode=git_worktree|in_place]\n"
        << "  agentos autodev status job_id=<job_id>\n"
        << "  agentos autodev status job_id=<job_id> --watch [iterations=1] [interval_ms=1000]\n"
        << "  agentos autodev watch job_id=<job_id> [iterations=1] [interval_ms=1000]\n"
        << "  agentos autodev summary job_id=<job_id>\n"
        << "  agentos autodev prepare-workspace job_id=<job_id>\n"
        << "  agentos autodev load-skill-pack job_id=<job_id> [skill_pack_path=<path>]\n"
        << "  agentos autodev generate-goal-docs job_id=<job_id>\n"
        << "  agentos autodev validate-spec job_id=<job_id>\n"
        << "  agentos autodev approve-spec job_id=<job_id> spec_hash=<sha256> [spec_revision=rev-001]\n"
        << "  agentos autodev recover-blocked job_id=<job_id> [skill_pack_path=<path>]\n"
        << "  agentos autodev tasks job_id=<job_id>\n"
        << "  agentos autodev turns job_id=<job_id>\n"
        << "  agentos autodev snapshot-task job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev snapshots job_id=<job_id>\n"
        << "  agentos autodev rollback-soft job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev rollback-hard job_id=<job_id> task_id=<task_id> approval=hard_rollback_approved\n"
        << "  agentos autodev rollbacks job_id=<job_id>\n"
        << "  agentos autodev repairs job_id=<job_id>\n"
        << "  agentos autodev repair-next job_id=<job_id>\n"
        << "  agentos autodev repair-task job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev verify-task job_id=<job_id> task_id=<task_id> [related_turn_id=<turn_id>]\n"
        << "  agentos autodev verifications job_id=<job_id>\n"
        << "  agentos autodev diff-guard job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev diffs job_id=<job_id>\n"
        << "  agentos autodev acceptance-gate job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev acceptances job_id=<job_id>\n"
        << "  agentos autodev final-review job_id=<job_id>\n"
        << "  agentos autodev final-reviews job_id=<job_id>\n"
        << "  agentos autodev complete-job job_id=<job_id>\n"
        << "  agentos autodev pause job_id=<job_id>\n"
        << "  agentos autodev resume job_id=<job_id>\n"
        << "  agentos autodev cancel job_id=<job_id>\n"
        << "  agentos autodev cleanup-worktree job_id=<job_id>\n"
        << "  agentos autodev pr-summary job_id=<job_id>\n"
        << "  agentos autodev events job_id=<job_id>\n"
        << "  agentos autodev execute-next-task job_id=<job_id> [execution_adapter=codex_cli|codex_app_server] [codex_cli_command=<command>] [app_server_url=<url>]\n"
        << "  agentos cli-specs validate\n"
        << "  agentos diagnostics [format=text|json]\n"
        << "  agentos plugins\n"
        << "  agentos plugins validate\n"
        << "  agentos plugins health\n"
        << "  agentos plugins lifecycle\n"
        << "  agentos plugins inspect name=<plugin_name> [health=true]\n"
        << "  agentos plugins sessions [name=<plugin_name>]\n"
        << "  agentos plugins session-restart name=<plugin_name>\n"
        << "  agentos plugins session-close name=<plugin_name>\n"
        << "  agentos memory summary\n"
        << "  agentos memory stats\n"
        << "  agentos memory workflows\n"
        << "  agentos memory stored-workflows [enabled=true|false] [trigger_task_type=write_file] [source=promoted_candidate] [name_contains=write]\n"
        << "  agentos memory show-workflow <workflow_name>\n"
        << "  agentos memory update-workflow <workflow_name> [new_name=<stored_name>] [trigger_task_type=write_file] [steps=file_write,file_read] [enabled=true|false] [required_inputs=a,b] [input_equals=mode=fast]\n"
        << "  agentos memory clone-workflow <workflow_name> new_name=<stored_name>\n"
        << "  agentos memory set-workflow-enabled <workflow_name> enabled=true|false\n"
        << "  agentos memory remove-workflow <workflow_name>\n"
        << "  agentos memory validate-workflows\n"
        << "  agentos memory explain-workflow <workflow_name> [task_type=<task_type>] key=value ...\n"
        << "  agentos memory lessons\n"
        << "  agentos memory promote-workflow <candidate_name> [workflow=<stored_name>] [required_inputs=a,b] [input_equals=mode=fast,status=ready] [input_number_gte=priority=5] [input_number_lte=size=10] [input_bool=approved=true] [input_regex=path=src/.*] [input_any=equals:mode=fast|equals:mode=safe] [input_expr=\"equals:mode=fast&&(exists:ticket||regex:branch=release/.*)\"]\n"
        << "  agentos storage status\n"
        << "  agentos storage backups\n"
        << "  agentos storage restore-backup name=<backup_name>\n"
        << "  agentos storage migrate\n"
        << "  agentos storage export dest=<directory>\n"
        << "  agentos storage import src=<directory>\n"
        << "  agentos storage recover\n"
        << "  agentos storage compact [target=all|memory|scheduler|audit]\n"
        << "  agentos schedule add task=<task_type> due=now [profile=name] [recurrence=every:5m|cron:<expr>] [cron=\"*/5 * * * *\"|@hourly|@daily|@weekly|@monthly|@yearly] [missed_run_policy=run-once|skip] key=value ...\n"
        << "  agentos schedule list\n"
        << "  agentos schedule history\n"
        << "  agentos schedule run-due\n"
        << "  agentos schedule tick [iterations=1] [interval_ms=1000]\n"
        << "  agentos schedule daemon [iterations=0] [interval_ms=1000]\n"
        << "  agentos schedule remove id=<schedule_id>\n"
        << "  agentos subagents run [agents=<agent[,agent]>] [mode=sequential|parallel] [profile=name] objective=text\n"
        << "  agentos trust identity-add identity=<id> [user=<user>] [label=name]\n"
        << "  agentos trust identities\n"
        << "  agentos trust identity-remove identity=<id>\n"
        << "  agentos trust invite-create identity=<id> device=<id> [label=name] [user=<user>] [identity_label=name] [permissions=task.submit] [ttl_seconds=600]\n"
        << "  agentos trust invite-accept token=<token>\n"
        << "  agentos trust invites\n"
        << "  agentos trust pair identity=<id> device=<id> [label=name] [permissions=task.submit]\n"
        << "  agentos trust list\n"
        << "  agentos trust role-set role=<name> permissions=filesystem.read,agent.invoke\n"
        << "  agentos trust role-show role=<name>\n"
        << "  agentos trust user-role user=<user> roles=<role1,role2>\n"
        << "  agentos trust user-role-show user=<user>\n"
        << "  agentos trust role-remove role=<name>\n"
        << "  agentos trust user-role-remove user=<user>\n"
        << "  agentos trust roles\n"
        << "  agentos trust approval-request subject=<text> [reason=text] [requested_by=user]\n"
        << "  agentos trust approval-show approval=<id>\n"
        << "  agentos trust approval-approve approval=<id> [approved_by=user]\n"
        << "  agentos trust approval-revoke approval=<id> [approved_by=user]\n"
        << "  agentos trust approvals\n"
        << "  agentos trust device-label identity=<id> device=<id> label=<name>\n"
        << "  agentos trust device-show identity=<id> device=<id>\n"
        << "  agentos trust device-seen identity=<id> device=<id>\n"
        << "  agentos trust unblock identity=<id> device=<id>\n"
        << "  agentos trust block identity=<id> device=<id>\n"
        << "  agentos trust remove identity=<id> device=<id>\n"
        << "  agentos auth providers\n"
        << "  agentos auth profiles [provider]\n"
        << "  agentos auth credential-store\n"
        << "  agentos auth status [provider] [profile=name]\n"
        << "  agentos auth oauth-defaults [provider]\n"
        << "  agentos auth oauth-config-validate\n"
        << "  agentos auth oauth-start <provider> client_id=ID redirect_uri=URL [authorization_endpoint=URL] [scopes=a,b] [profile=name] [open_browser=true]\n"
        << "  agentos auth oauth-login <provider> client_id=ID redirect_uri=URL [authorization_endpoint=URL] [token_endpoint=URL] [scopes=a,b] [profile=name] [set_default=true] [port=48177] [timeout_ms=120000] [open_browser=true]\n"
        << "  agentos auth oauth-callback callback_url=URL state=STATE\n"
        << "  agentos auth oauth-listen state=STATE port=48177 [timeout_ms=120000]\n"
        << "  agentos auth oauth-complete <provider> callback_url=URL state=STATE code_verifier=VERIFIER redirect_uri=URL client_id=ID [token_endpoint=URL] [profile=name] [account_label=label] [set_default=true]\n"
        << "  agentos auth oauth-token-request token_endpoint=URL client_id=ID redirect_uri=URL code=CODE code_verifier=VERIFIER\n"
        << "  agentos auth oauth-refresh-request token_endpoint=URL client_id=ID refresh_token=TOKEN\n"
        << "  agentos auth login <provider> mode=api-key api_key_env=ENV_NAME [profile=name] [set_default=true]\n"
        << "  agentos auth login <provider> mode=cli-session [profile=name] [set_default=true]\n"
        << "  agentos auth default-profile <provider> profile=name\n"
        << "  agentos auth refresh <provider> [profile=name]\n"
        << "  agentos auth probe <provider>\n"
        << "  agentos run <task_type> [profile=name] key=value ...\n\n"
        << "Examples:\n"
        << "  agentos run read_file path=README.md\n"
        << "  agentos run write_file path=runtime/note.txt content=hello idempotency_key=demo-write-1\n"
        << "  agentos memory promote-workflow write_file_workflow\n"
        << "  agentos memory show-workflow write_file_workflow\n"
        << "  agentos storage status\n"
        << "  agentos storage backups\n"
        << "  agentos storage restore-backup name=import-123\n"
        << "  agentos storage migrate\n"
        << "  agentos storage export dest=runtime_export\n"
        << "  agentos storage import src=runtime_export\n"
        << "  agentos storage recover\n"
        << "  agentos storage compact target=all\n"
        << "  agentos schedule add id=demo-once task=write_file due=now path=runtime/scheduled.txt content=hello\n"
        << "  agentos schedule run-due\n"
        << "  agentos schedule tick iterations=1 interval_ms=0\n"
        << "  agentos schedule daemon iterations=1 interval_ms=0\n"
        << "  agentos subagents run agents=local_planner mode=sequential objective=Plan_the_next_phase\n"
        << "  agentos trust identity-add identity=phone user=local-user label=dev-phone\n"
        << "  agentos trust invite-create identity=phone device=device1 label=dev-phone permissions=task.submit\n"
        << "  agentos trust pair identity=phone device=device1 label=dev-phone permissions=task.submit\n"
        << "  agentos run read_file path=README.md remote=true origin_identity=phone origin_device=device1\n"
        << "  agentos run workflow_run workflow=write_patch_read path=runtime/wf.txt content=hello find=hello replace=done idempotency_key=demo-wf-1\n"
        << "  agentos run rg_search pattern=AgentOS path=README.md\n"
        << "  agentos run http_fetch url=https://example.com allow_network=true\n"
        << "  agentos run read_file path=README.md permission_grants=filesystem.read\n"
        << "  agentos run custom_high_risk allow_high_risk=true approval_id=approval-123\n"
        << "  agentos run analysis target=codex_cli objective=Review_the_project_structure\n"
        << "  agentos run analysis target=gemini profile=work objective=Use_a_non_default_auth_profile\n"
        << "  agentos run analysis target=local_planner objective=Design_the_next_phase\n";
}

}  // namespace

}  // namespace agentos

// Resolve the agentos workspace.
//
// Resolution order:
//   1. AGENTOS_WORKSPACE env var (must point at a directory).
//   2. Walk up from cwd looking for a directory that already contains
//      runtime/auth_sessions.tsv — if cwd is e.g. <repo>/build/, this
//      finds <repo>/ and reuses its auth/audit/scheduler state instead
//      of creating a fresh empty runtime/ alongside the binary.
//   3. Fall back to cwd.
//
// This matters because `runtime/` (auth sessions, audit logs, scheduler
// state) is resolved relative to the workspace. Launching the binary
// from build/ used to look like every authed agent went unhealthy.
std::filesystem::path ResolveWorkspace() {
    namespace fs = std::filesystem;
    std::string env_value;
#ifdef _WIN32
    char* env_buf = nullptr;
    size_t env_len = 0;
    if (_dupenv_s(&env_buf, &env_len, "AGENTOS_WORKSPACE") == 0 && env_buf) {
        env_value.assign(env_buf);
        free(env_buf);
    }
#else
    if (const char* env = std::getenv("AGENTOS_WORKSPACE"); env != nullptr) {
        env_value.assign(env);
    }
#endif
    if (!env_value.empty()) {
        std::error_code ec;
        const fs::path candidate = fs::absolute(env_value, ec);
        if (!ec && fs::is_directory(candidate, ec)) {
            return candidate;
        }
        std::cerr << "AGENTOS_WORKSPACE=" << env_value
                  << " is not a directory; falling back to auto-detection.\n";
    }

    std::error_code ec;
    fs::path cursor = fs::current_path(ec);
    if (!ec) {
        const fs::path start = cursor;
        for (int i = 0; i < 8; ++i) {
            const fs::path marker = cursor / "runtime" / "auth_sessions.tsv";
            if (fs::exists(marker, ec)) {
                if (cursor != start) {
                    std::cerr << "agentos: using workspace " << cursor.string()
                              << " (found existing runtime/auth_sessions.tsv via parent walk; "
                              << "set AGENTOS_WORKSPACE to override)\n";
                }
                return cursor;
            }
            const fs::path parent = cursor.parent_path();
            if (parent == cursor) {
                break;
            }
            cursor = parent;
        }
    }

    return fs::current_path();
}

int main(int argc, char* argv[]) {
    using namespace agentos;

    const auto workspace = ResolveWorkspace();
    Runtime runtime(workspace);

    runtime.skill_registry.register_skill(std::make_shared<FileReadSkill>());
    runtime.skill_registry.register_skill(std::make_shared<FileWriteSkill>());
    runtime.skill_registry.register_skill(std::make_shared<FilePatchSkill>());
    runtime.skill_registry.register_skill(std::make_shared<HttpFetchSkill>(runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<NewsSearchSkill>(runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<HostInfoSkill>());
    runtime.skill_registry.register_skill(std::make_shared<LearnSkill>(
        runtime.skill_registry, runtime.cli_host, runtime.plugin_host,
        runtime.audit_logger, workspace));
    runtime.skill_registry.register_skill(std::make_shared<DevelopmentSkill>(
        runtime.agent_registry, runtime.loop, runtime.audit_logger, workspace));
    runtime.skill_registry.register_skill(std::make_shared<ResearchSkill>(
        runtime.agent_registry, runtime.loop, runtime.audit_logger, workspace));
    runtime.skill_registry.register_skill(std::make_shared<WorkflowRunSkill>(
        runtime.skill_registry, &runtime.memory_manager.workflow_store()));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeRgSearchSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeGitStatusSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeGitDiffSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeCurlFetchSpec(), runtime.cli_host));
    runtime.skill_registry.register_skill(std::make_shared<CliSkillInvoker>(MakeJqTransformSpec(), runtime.cli_host));
    RegisterExternalCliSpecs(runtime, workspace);
    RegisterPluginSpecs(runtime, workspace);
    runtime.agent_registry.register_agent(std::make_shared<MainAgent>(
        runtime.cli_host, runtime.main_agent_store, workspace,
        runtime.skill_registry, runtime.agent_registry));
    runtime.agent_registry.register_agent(std::make_shared<LocalPlanningAgent>());
    runtime.agent_registry.register_agent(std::make_shared<GeminiAgent>(
        runtime.cli_host, runtime.credential_broker, runtime.auth_profile_store, workspace));
    runtime.agent_registry.register_agent(std::make_shared<AnthropicAgent>(
        runtime.cli_host, runtime.credential_broker, runtime.auth_profile_store, workspace));
    runtime.agent_registry.register_agent(std::make_shared<QwenAgent>(
        runtime.cli_host, runtime.credential_broker, runtime.auth_profile_store, workspace,
        &runtime.skill_registry, &runtime.agent_registry));
    runtime.agent_registry.register_agent(std::make_shared<OpenAiAgent>(
        runtime.cli_host, runtime.credential_broker, runtime.auth_profile_store, workspace));
    runtime.agent_registry.register_agent(std::make_shared<CodexCliAgent>(runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<OpenAiAuthProviderAdapter>(
        runtime.session_store, runtime.token_store, runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<GeminiAuthProviderAdapter>(
        runtime.session_store, runtime.token_store, runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<AnthropicAuthProviderAdapter>(
        runtime.session_store, runtime.token_store, runtime.cli_host, workspace));
    runtime.auth_manager.register_provider(std::make_shared<QwenAuthProviderAdapter>(
        runtime.session_store, runtime.token_store));

    if (argc >= 2 && std::string(argv[1]) == "demo") {
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
        return RunAgentsCommand(runtime.agent_registry);
    }

    if (argc >= 2 && std::string(argv[1]) == "autodev") {
        return RunAutoDevCommand(workspace, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "cli-specs") {
        return RunCliSpecsCommand(workspace, BuiltinSkillNames(), argc, argv);
    }

    if (argc == 1 || (argc >= 2 && std::string(argv[1]) == "interactive")) {
        return RunInteractiveCommand(
            runtime.skill_registry,
            runtime.agent_registry,
            runtime.loop,
            runtime.memory_manager,
            runtime.scheduler,
            runtime.audit_logger,
            workspace,
            argc,
            argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "serve") {
        return RunServeCommand(
            runtime.skill_registry,
            runtime.agent_registry,
            runtime.loop,
            runtime.memory_manager,
            runtime.scheduler,
            runtime.audit_logger,
            workspace,
            argc,
            argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "diagnostics") {
        return RunDiagnosticsCommand(
            workspace,
            runtime.skill_registry,
            runtime.agent_registry,
            runtime.auth_manager,
            runtime.session_store,
            runtime.auth_profile_store,
            runtime.token_store,
            runtime.plugin_host,
            runtime.scheduler,
            runtime.storage_version_store,
            runtime.identity_manager,
            runtime.allowlist_store,
            runtime.pairing_invite_store,
            runtime.role_catalog,
            runtime.approval_store,
            argc,
            argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "plugins") {
        return RunPluginsCommand(workspace, BuiltinSkillNames(), argc, argv, &runtime.plugin_host);
    }

    if (argc >= 2 && std::string(argv[1]) == "auth") {
        return RunAuthCommand(runtime.auth_manager, runtime.session_store, runtime.token_store, runtime.cli_host, workspace, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "main-agent") {
        return RunMainAgentCommand(runtime.main_agent_store, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "memory") {
        return RunMemoryCommand(runtime.memory_manager, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "storage") {
        return RunStorageCommand(
            runtime.storage_version_store,
            runtime.session_store,
            runtime.auth_profile_store,
            runtime.execution_cache,
            runtime.memory_manager,
            runtime.scheduler,
            runtime.audit_logger,
            argc,
            argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "schedule") {
        return RunScheduleCommand(runtime.scheduler, runtime.loop, workspace, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "subagents") {
        return RunSubagentsCommand(runtime.subagent_manager, workspace, argc, argv);
    }

    if (argc >= 2 && std::string(argv[1]) == "trust") {
        return agentos::RunTrustCommand(
            runtime.identity_manager,
            runtime.pairing_invite_store,
            runtime.pairing_manager,
            runtime.role_catalog,
            runtime.approval_store,
            runtime.audit_logger,
            argc,
            argv);
    }

    if (argc >= 3 && std::string(argv[1]) == "run") {
        const auto task = BuildTaskFromArgs(argc, argv, workspace);
        // Install Ctrl-C / SIGINT handler so a long agent dispatch can be
        // interrupted cooperatively. Skill-routed runs ignore the token.
        auto cancel = agentos::InstallSignalCancellation();
        const auto result = runtime.loop.run(task, std::move(cancel));
        PrintResult(result);
        std::cout << "audit_log: " << runtime.audit_logger.log_path().string() << '\n';
        return result.success ? 0 : 1;
    }

    PrintUsage();
    return 1;
}
