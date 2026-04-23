#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

using StringMap = std::unordered_map<std::string, std::string>;

struct SkillManifest {
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> capabilities;
    std::string input_schema_json;
    std::string output_schema_json;
    std::string risk_level;
    std::vector<std::string> permissions;
    bool supports_streaming = false;
    bool idempotent = false;
    int timeout_ms = 0;
};

struct SkillCall {
    std::string call_id;
    std::string skill_name;
    std::string json_args;
    std::string workspace_id;
    std::string user_id;
    std::string idempotency_key;
    StringMap arguments;

    std::optional<std::string> get_arg(const std::string& key) const {
        const auto it = arguments.find(key);
        if (it == arguments.end()) {
            return std::nullopt;
        }
        return it->second;
    }
};

struct SkillResult {
    bool success = false;
    std::string json_output;
    std::string error_code;
    std::string error_message;
    int duration_ms = 0;
};

class ISkillAdapter {
public:
    virtual SkillManifest manifest() const = 0;
    virtual SkillResult execute(const SkillCall& call) = 0;
    virtual bool healthy() const = 0;
    virtual ~ISkillAdapter() = default;
};

struct AgentCapability {
    std::string name;
    int score = 0;
};

struct AgentProfile {
    std::string agent_name;
    std::string version;
    std::string description;
    std::vector<AgentCapability> capabilities;
    bool supports_session = false;
    bool supports_streaming = false;
    bool supports_patch = false;
    bool supports_subagents = false;
    bool supports_network = false;
    std::string cost_tier;
    std::string latency_tier;
    std::string risk_level;
};

struct AgentTask {
    std::string task_id;
    std::string task_type;
    std::string objective;
    std::string workspace_path;
    std::string context_json;
    std::string constraints_json;
    int timeout_ms = 0;
    double budget_limit = 0.0;
};

struct AgentArtifact {
    std::string type;
    std::string uri;
    std::string content;
    std::string metadata_json;
};

struct AgentResult {
    bool success = false;
    std::string summary;
    std::string structured_output_json;
    std::vector<AgentArtifact> artifacts;
    int duration_ms = 0;
    double estimated_cost = 0.0;
    std::string error_code;
    std::string error_message;
};

class IAgentAdapter {
public:
    virtual AgentProfile profile() const = 0;
    virtual bool healthy() const = 0;
    virtual std::string start_session(const std::string& session_config_json) = 0;
    virtual void close_session(const std::string& session_id) = 0;
    virtual AgentResult run_task(const AgentTask& task) = 0;
    virtual AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) = 0;
    virtual bool cancel(const std::string& task_id) = 0;
    virtual ~IAgentAdapter() = default;
};

enum class RouteTargetKind {
    none,
    skill,
    agent,
};

inline std::string route_target_kind_name(const RouteTargetKind kind) {
    switch (kind) {
    case RouteTargetKind::skill:
        return "skill";
    case RouteTargetKind::agent:
        return "agent";
    case RouteTargetKind::none:
    default:
        return "none";
    }
}

struct RouteDecision {
    RouteTargetKind target_kind = RouteTargetKind::none;
    std::string target_name;
    std::string rationale;

    [[nodiscard]] bool found() const {
        return target_kind != RouteTargetKind::none;
    }
};

struct PolicyDecision {
    bool allowed = false;
    std::string reason;
};

struct TaskRequest {
    std::string task_id;
    std::string task_type;
    std::string objective;
    std::filesystem::path workspace_path;
    std::string user_id = "local-user";
    std::string idempotency_key;
    bool remote_trigger = false;
    std::string origin_identity_id;
    std::string origin_device_id;
    StringMap inputs;
    std::optional<std::string> preferred_target;
    int timeout_ms = 5000;
    double budget_limit = 0.0;
    bool allow_high_risk = false;
    bool allow_network = false;
};

struct TaskStepRecord {
    RouteTargetKind target_kind = RouteTargetKind::none;
    std::string target_name;
    bool success = false;
    int duration_ms = 0;
    std::string summary;
    std::string error_code;
    std::string error_message;
};

struct TaskRunResult {
    bool success = false;
    bool from_cache = false;
    std::string summary;
    std::string route_target;
    RouteTargetKind route_kind = RouteTargetKind::none;
    std::string output_json;
    std::string error_code;
    std::string error_message;
    int duration_ms = 0;
    std::vector<TaskStepRecord> steps;
};

struct SkillStats {
    int total_calls = 0;
    int success_calls = 0;
    double avg_latency_ms = 0.0;
    double avg_cost = 0.0;
    double acceptance_rate = 0.0;
};

struct AgentRuntimeStats {
    int total_runs = 0;
    int success_runs = 0;
    int failed_runs = 0;
    double avg_duration_ms = 0.0;
    double avg_cost = 0.0;
    double avg_user_score = 0.0;
    double patch_accept_rate = 0.0;
};

struct WorkflowCandidate {
    std::string name;
    std::string trigger_task_type;
    std::vector<std::string> ordered_steps;
    int use_count = 0;
    int success_count = 0;
    int failure_count = 0;
    double success_rate = 0.0;
    double avg_duration_ms = 0.0;
    double score = 0.0;
};

}  // namespace agentos
