#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

using StringMap = std::unordered_map<std::string, std::string>;

class CancellationToken;  // forward declaration; full definition in utils/cancellation.hpp

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
    // Phase 3 V2 additions — coexist with the legacy fields above; older
    // adapters that fill type/uri/content/metadata_json keep working.
    std::string mime;                        // explicit MIME, decoupled from `type`
    std::vector<std::byte> inline_bytes;     // binary-safe payload (replaces stuffing bytes into `content`)
    StringMap metadata;                      // typed key/value metadata (replaces metadata_json string)
};

// Phase 3 — usage actually measured by the upstream API/CLI, not estimated.
// Wired into AgentResult.usage; orchestrator accumulates these to drive the
// V2 admission-control budget gate.
struct AgentUsage {
    int input_tokens = 0;
    int output_tokens = 0;
    int reasoning_tokens = 0;
    double cost_usd = 0.0;
    int turns = 0;
    StringMap per_model;
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
    // Phase 3 V2 additions.
    AgentUsage usage;                         // fed by upstream Usage events; non-zero only on V2 path
    std::optional<std::string> session_id;    // service-side session token returned for follow-up turns
    bool from_stream_fallback = false;        // true if streaming failed and the orchestrator retried sync
};

// Phase 3 — explicit invocation context replaces the opaque AgentTask
// context_json/constraints_json blobs and removes the need for the adapter
// to maintain its own task_id -> cancel-state map.
struct AgentInvocation {
    std::string task_id;
    std::string objective;
    std::filesystem::path workspace_path;
    StringMap context;                              // structured replacement for context_json
    StringMap constraints;                          // structured replacement for constraints_json
    std::optional<std::string> session_id;          // continue an existing kernel-issued session
    std::optional<std::string> resume_session_id;   // upstream-side session id (e.g. `codex --resume <id>`)
    std::vector<std::string> attachments;           // file paths the adapter may inline or upload
    int timeout_ms = 0;
    double budget_limit_usd = 0.0;
    std::shared_ptr<CancellationToken> cancel;      // shared signal; replaces IAgentAdapter::cancel(task_id)
};

// Phase 3 — normalized event union the adapter emits during a streaming
// invoke. Inspired by ductor's stream_events.py but tied into the AgentOS
// kernel's policy/audit/budget hooks rather than just observability.
struct AgentEvent {
    enum class Kind {
        SessionInit,       // upstream-side session started; fields: session_id, model, version
        TextDelta,         // assistant text fragment; payload_text carries the chunk
        Thinking,          // reasoning chunk (UI may collapse); payload_text carries the chunk
        ToolUseStart,      // adapter wants to invoke a tool; fields: tool_name, args_json
        ToolUseResult,     // tool returned; fields: tool_name, success, output_json
        Status,            // human-readable progress; payload_text carries the message
        CompactBoundary,   // upstream context was compacted; fields: trigger, pre_tokens, post_tokens
        Usage,             // incremental usage delta; fields: input_tokens, output_tokens, cost_usd
        Final,             // no more events follow; the AgentResult is also returned synchronously
        Error,             // error mid-stream; fields: error_code, error_message
    };
    Kind kind = Kind::Status;
    StringMap fields;        // simple typed fields; ADR-JSON-001 may upgrade to nlohmann::json later
    std::string payload_text; // chunk body for TextDelta / Thinking / Status
};

// Returning `false` from the callback signals the orchestrator wants to cancel:
// the adapter should call `invocation.cancel->cancel()` (or equivalent) and
// return as quickly as possible. The orchestrator may also have already
// triggered the cancel itself before the callback fires.
using AgentEventCallback = std::function<bool(const AgentEvent&)>;

// Phase 3 — V2 single-entry adapter interface. Coexists with the legacy
// IAgentAdapter via dynamic_cast in SubagentManager during the staged
// migration (Phase 4). When the callback is empty, adapters return a
// synchronous AgentResult lump exactly like the legacy path; when present,
// adapters emit AgentEvents in real time so the kernel can apply admission
// control (budget, policy, audit) before each upstream tool call or token
// charge.
class IAgentAdapterV2 {
public:
    virtual ~IAgentAdapterV2() = default;
    virtual AgentProfile profile() const = 0;
    virtual bool healthy() const = 0;
    virtual AgentResult invoke(
        const AgentInvocation& invocation,
        const AgentEventCallback& on_event = {}) = 0;
    // Defaults: adapters that do not support persistent sessions get the
    // empty implementation for free; SubagentManager treats nullopt as
    // "session not supported, fall back to one-shot invoke()".
    virtual std::optional<std::string> open_session(const StringMap& config) {
        (void)config;
        return std::nullopt;
    }
    virtual void close_session(const std::string& /*session_id*/) {}
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
    std::optional<std::string> workflow_name;

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
    std::string approval_id;
    std::vector<std::string> permission_grants;
};

struct TaskStepRecord {
    RouteTargetKind target_kind = RouteTargetKind::none;
    std::string target_name;
    bool success = false;
    int duration_ms = 0;
    double estimated_cost = 0.0;
    std::string summary;
    std::string structured_output_json;
    std::vector<AgentArtifact> artifacts;
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
    // Maps "step1|step2|..." signature -> count of successful runs that
    // produced exactly that step sequence. The Phase 2.2 promotion gate
    // requires a single signature to recur (>= 3 occurrences AND >= 60%
    // of all successful runs of this task_type) before the workflow is
    // canonicalized; this map carries the evidence for that decision and
    // is persisted in workflow_candidates.tsv schema v2.
    std::unordered_map<std::string, int> step_signature_counts;
};

}  // namespace agentos
