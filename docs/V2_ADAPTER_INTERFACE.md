# V2 Adapter Interface

## 1. Overview

`IAgentAdapterV2` is the streaming-aware successor to `IAgentAdapter`. It
collapses the legacy three-method dispatch surface
(`start_session` / `run_task` / `run_task_in_session` / `cancel`) into a single
`invoke()` entry point that:

- accepts a structured `AgentInvocation` (typed fields instead of opaque
  `context_json` / `constraints_json` blobs),
- emits a stream of normalized `AgentEvent` records via an
  `AgentEventCallback`, so the orchestrator can apply admission control
  (budget, policy, audit) before each upstream tool call or token charge,
- carries a shared `CancellationToken` instead of forcing every adapter to
  maintain its own `task_id` -> cancel-state map,
- reports measured `AgentUsage` (input/output/reasoning tokens, cost, turns,
  per-model breakdown) on the returned `AgentResult`, so the V2 budget gate
  has real numbers rather than estimates.

V1 and V2 coexist deliberately. The migration is staged: every existing call
site that still expects `IAgentAdapter` keeps working, while V2-aware callers
(`SubagentManager::run_one`, `AgentLoop::run_agent_task`) `dynamic_cast` to
`IAgentAdapterV2*` and prefer `invoke()` when the cast succeeds. The legacy
interface will not be removed until all callers and tests are migrated.

When should you write a V2 adapter?

- The upstream provider exposes a streaming protocol (SSE, NDJSON, or chunked
  JSON-RPC) and you want the kernel to observe deltas as they arrive — for
  budget gating, live UI, or compaction.
- The provider returns real token / cost numbers and you want them flowing
  into `step.estimated_cost` and `MemoryManager::agent_stats` without a
  conversion step.
- You want cooperative cancellation (Ctrl-C, parent task timeout, sibling
  failure) to interrupt the call mid-flight, not only at dispatch time.

If none of those apply, the V1 path is still acceptable. The dual-inheritance
pattern in the existing migrated adapters lets you grow into V2 incrementally
rather than rewriting up front.

---

## 2. Type reference

All declarations live in `src/core/models.hpp`. The cancellation primitive is
defined in `src/utils/cancellation.hpp`.

### 2.1 IAgentAdapterV2

```cpp
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
```

- `profile()` / `healthy()` keep the same signatures as `IAgentAdapter`, so
  dual-inherited adapters override them once.
- `invoke()` is the single dispatch entry. When `on_event` is empty the
  adapter still returns a complete `AgentResult` synchronously — the same
  contract as `run_task()`. When `on_event` is non-empty, the adapter must
  call it for each `AgentEvent` it produces and respect a `false` return to
  cancel.
- `open_session()` returns `std::nullopt` by default. Adapters that support
  persistent sessions (e.g. `LocalPlanningAgent`, `QwenAgent`) override it.
- `close_session()` matches the V1 signature. A single override satisfies
  both vtables.

### 2.2 AgentInvocation

```cpp
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
```

- `task_id` matches the orchestrator-side identifier; subagent calls
  decorate it as `parent_task_id + "." + agent_name` (see
  `SubagentManager::run_one`).
- `objective` is the natural-language task statement. Subagent calls prefix
  it with `"[" + role + "] "`.
- `workspace_path` is a `std::filesystem::path`, not a string — adapters
  that need a string call `.string()` once.
- `context` carries kernel-assigned key/value pairs (e.g. `task_type`,
  `parent_task_id`, `agent`, `role`, `original_objective`,
  `subtask_objective`). It replaces the legacy `context_json` blob.
- `constraints` carries caller-supplied controls (e.g. `model`). It
  replaces `constraints_json`.
- `session_id` / `resume_session_id` distinguish a kernel-issued session
  handle (`local-session-1`, etc.) from an upstream-side resumption
  identifier (`codex --resume <id>`).
- `attachments` is reserved for file paths the adapter may inline or
  upload. Currently unused by the migrated adapters.
- `cancel` is the shared signal (see §2.6). It is `nullptr` when the caller
  did not opt in.

### 2.3 AgentEvent and AgentEvent::Kind

```cpp
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
```

`fields` is a `StringMap` for now (ADR-JSON-001 may upgrade to
`nlohmann::json` once the JSON dependency is universally adopted).
`payload_text` carries the bulk text for `TextDelta`, `Thinking`, and
`Status`; it is empty for the structured kinds. The exact field keys per
`Kind` are documented inline in the enum comments above.

### 2.4 AgentEventCallback

```cpp
// Returning `false` from the callback signals the orchestrator wants to cancel:
// the adapter should call `invocation.cancel->cancel()` (or equivalent) and
// return as quickly as possible. The orchestrator may also have already
// triggered the cancel itself before the callback fires.
using AgentEventCallback = std::function<bool(const AgentEvent&)>;
```

The callback contract is one-line important: returning `false` is a request
to cancel. The kernel uses this when the budget gate trips, when policy
denies a mid-stream tool call, or when a parent task is cancelled. Adapters
must not ignore it.

### 2.5 AgentUsage and AgentResult additions

```cpp
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
```

```cpp
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
```

Notes:

- `usage.cost_usd` is the measured cost. Orchestrator routing prefers it
  over `estimated_cost` whenever it is greater than zero (see §6).
- `usage.turns` is the count of round-trips with the upstream model. Even
  zero-cost adapters (`LocalPlanningAgent`, `GeminiAgent` today) set
  `turns = 1` so the budget gate sees explicit data rather than missing
  fields.
- `session_id` lets `SubagentManager` reuse an upstream session across
  follow-up calls.
- `from_stream_fallback` is set by adapters that attempted streaming, hit
  an early failure (e.g. curl missing, secret staging failed, upstream
  returned non-200 before the first event), and retried via the
  non-streaming path. The kernel uses it to decide whether to penalize the
  adapter's streaming health.

### 2.6 CancellationToken

Defined in `src/utils/cancellation.hpp`:

```cpp
// Shared cancellation signal passed by reference to long-running operations.
// One producer (the orchestrator) calls cancel(); many observers may poll
// is_cancelled() or wait().
//
// Replaces the old IAgentAdapter::cancel(task_id) method, which forced every
// adapter to maintain a private task_id -> state map.
class CancellationToken {
public:
    void cancel() noexcept;
    [[nodiscard]] bool is_cancelled() const noexcept;

    // Blocks until cancel() is called or `timeout` elapses. Returns true if
    // cancelled before timeout, false on timeout. Useful for adapters that
    // want to interrupt their own wait loops promptly.
    bool wait_for_cancel(std::chrono::milliseconds timeout) const;

private:
    std::atomic<bool> cancelled_{false};
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
};
```

Adapters poll `is_cancelled()` between expensive steps, or call
`wait_for_cancel(timeout)` instead of plain `sleep_for` so they unblock
immediately when cancellation fires.

---

## 3. Event sequence

The canonical event order for a successful streaming call is:

```
SessionInit  -> (Status*  TextDelta*  Thinking*  ToolUseStart  ToolUseResult)*  Usage*  Final
```

Rules of thumb:

- `SessionInit` first, with `fields["session_id"]` and (where available)
  `fields["model"]` and `fields["version"]`. Some adapters also include
  `fields["agent"]` and `fields["profile"]`.
- `Status` events are free-form progress (`phase=planning`, `phase=dispatch`,
  etc.). They are the safe fallback for upstream payloads the adapter cannot
  classify.
- `TextDelta` carries the assistant chunk in `payload_text`; the adapter
  also accumulates it locally so the final `AgentResult.summary` is the full
  text.
- `Thinking` is reasoning content the UI may collapse. `payload_text` again.
- `ToolUseStart` / `ToolUseResult` use `fields["tool_name"]`,
  `fields["args_json"]`, `fields["success"]`, `fields["output_json"]`.
- `Usage` may be emitted multiple times (incremental deltas); the adapter
  also accumulates into `AgentResult.usage`. Fields:
  `input_tokens`, `output_tokens`, `reasoning_tokens`, `cost_usd`, `turns`.
- `Final` is always the last event. `payload_text` is the final assistant
  text; `fields["success"]` is `"true"` or `"false"`. The adapter still
  returns a fully populated `AgentResult` synchronously — `Final` is for the
  stream observer, not the structured caller.
- `Error` may interrupt the sequence. Fields: `error_code`, `error_message`.
  The adapter still returns an `AgentResult` with `success = false` and the
  matching `error_code` / `error_message`.
- `CompactBoundary` is emitted by adapters that observe upstream context
  compaction (`fields`: `trigger`, `pre_tokens`, `post_tokens`). None of the
  current adapters surface it; it is reserved for future Anthropic / Codex
  releases.

Concrete adapter coverage today:

- `CodexCliAgent` (`src/hosts/agents/codex_cli_agent.cpp`) has the most
  complete mapping. It spawns `codex` directly, parses one NDJSON object
  per stdout line, and emits the full set of kinds — `SessionInit`,
  `TextDelta`, `Thinking`, `ToolUseStart`, `ToolUseResult`, `Usage`,
  `Final`, `Error`, plus a `Status` fallback for unknown payload types.
- `QwenAgent` (`src/hosts/agents/qwen_agent.cpp`) parses the DashScope
  OpenAI-compatible SSE stream and emits `SessionInit`, `Thinking`,
  `TextDelta`, `Usage`, and `Final` (plus `Error` / `Status` on failure).
- `AnthropicAgent` (`src/hosts/agents/anthropic_agent.cpp`) drives the
  Anthropic Messages REST endpoint with `stream=true`, parses the SSE
  frames, and emits `SessionInit`, `Usage` (initial input-token count),
  `TextDelta`, `Thinking`, `Final`, and `Error`. On early failure it
  returns `nullopt` from `invoke_with_rest_streaming` and the caller falls
  back to the non-streaming path with `from_stream_fallback = true`.
- `LocalPlanningAgent` (`src/hosts/agents/local_planning_agent.cpp`) is a
  lightweight wrap. It emits `SessionInit` (only when a session id is
  present), one `Status` (`phase=planning`), one `Usage` with zero cost,
  and `Final`. There is no upstream stream to forward.
- `GeminiAgent` (`src/hosts/agents/gemini_agent.cpp`) is also a sync wrap
  today. It emits `SessionInit`, one `Status` (`phase=dispatch`), runs the
  legacy `run_task` REST/CLI path, then emits a zero-cost `Usage` and
  `Final`. Mid-call interruption is not yet supported.

---

## 4. Cancellation contract

There are three observers of cancellation in a V2 invocation:

1. **The orchestrator-scoped token.** `InstallSignalCancellation()`
   (`src/utils/signal_cancellation.hpp`) installs a process-wide Ctrl-C /
   SIGINT (and SIGTERM on POSIX) handler that trips a single shared
   `CancellationToken`. The function is idempotent — subsequent calls return
   the same token without re-installing the handler. A second signal
   restores the OS default disposition and re-raises, so users always retain
   force-kill.

2. **The orchestrator's pre-dispatch checkpoints.** Both
   `AgentLoop::run_agent_task` and `SubagentManager::run_one` accept an
   optional `std::shared_ptr<CancellationToken>`. They check `is_cancelled()`
   before policy evaluation and again after, recording a
   `TaskStepRecord` with `error_code = "Cancelled"` on a tripped token.
   Sequential subagent execution short-circuits remaining agents the same
   way; parallel mode launches every future first and observes cancellation
   inside `run_one`.

3. **The adapter's mid-call polling.** `AgentInvocation::cancel` is the
   `shared_ptr<CancellationToken>` propagated from the orchestrator. Adapters
   should:
   - Check `invocation.cancel && invocation.cancel->is_cancelled()` at every
     natural break (before each upstream request, after each parsed event,
     between retries).
   - Use `wait_for_cancel(timeout)` instead of plain sleeps when waiting.
   - Honor a `false` return from `AgentEventCallback` as an additional
     cancel signal — the orchestrator may want to stop receiving events even
     if the token has not tripped yet.
   - Return promptly with an `AgentResult` whose `success = false` and
     `error_code = "Cancelled"`. The reference message text used by
     `LocalPlanningAgent` is `"local planning task was cancelled"`; pick
     something equivalent that names the adapter.

The `Cancelled` error code is the stable name. `SubagentManager::run_one`
and `AgentLoop::run_agent_task` use the same code for their pre-dispatch
short-circuits, so downstream consumers (audit, memory, lessons) see one
canonical value.

---

## 5. Migration recipe

`LocalPlanningAgent` is the canonical reference for migrating a legacy
adapter. Walk it step by step.

### 5.1 Dual inheritance

```cpp
// Phase 4.1 reference V2 adapter. Inherits both interfaces so legacy callers
// (SubagentManager, AgentRegistry) keep working through the IAgentAdapter
// shim while V2 callers can drive invoke()/CancellationToken directly.
// profile()/healthy()/close_session() share signatures across both interfaces,
// so a single override satisfies both bases.
class LocalPlanningAgent final : public IAgentAdapter, public IAgentAdapterV2 {
public:
    AgentProfile profile() const override;
    bool healthy() const override;

    // Legacy IAgentAdapter surface — translates to/from invoke().
    std::string start_session(const std::string& session_config_json) override;
    void close_session(const std::string& session_id) override;
    AgentResult run_task(const AgentTask& task) override;
    AgentResult run_task_in_session(const std::string& session_id, const AgentTask& task) override;
    bool cancel(const std::string& task_id) override;

    // V2 surface.
    AgentResult invoke(const AgentInvocation& invocation,
                       const AgentEventCallback& on_event = {}) override;
    std::optional<std::string> open_session(const StringMap& config) override;
    ...
};
```

Three signatures collide between the two interfaces and a single override
satisfies both vtables: `profile()`, `healthy()`, and
`close_session(const std::string&)`. Mark them `override` once. The other
methods belong to one interface or the other.

### 5.2 The translation helpers

The legacy `run_task` becomes a thin shim that builds an `AgentInvocation`
and calls `invoke()`:

```cpp
AgentResult LocalPlanningAgent::run_task(const AgentTask& task) {
    AgentInvocation invocation{
        .task_id = task.task_id,
        .objective = task.objective,
        .workspace_path = task.workspace_path,
        .context = {{"task_type", task.task_type}},
        .timeout_ms = task.timeout_ms,
        .budget_limit_usd = task.budget_limit,
    };
    return invoke(invocation);
}
```

For richer adapters, factor the translation into a static helper. Two
naming conventions are in use:

- `TaskFromInvocation(const AgentInvocation&)` — used by `AnthropicAgent`
  and `GeminiAgent` to feed the legacy `run_task` body when the V2 path
  needs to fall back.
- `InvocationToTask(const AgentInvocation&)` — used by `QwenAgent` for the
  same purpose.

Use whichever name reads better in your adapter; both walk
`invocation.context` / `invocation.constraints` into the corresponding JSON
strings on `AgentTask`. The richer adapters (`AnthropicAgent`, `QwenAgent`)
also expose paired prompt builders (`BuildPrompt` / `BuildPromptV2`) and
request body builders (`BuildRequestBody` / `BuildRequestBodyV2`) so the
two paths share scaffolding without one calling into the other.

A small `ModelNameFromConstraints(const StringMap&)` static is the
convention for picking the upstream model from
`invocation.constraints["model"]`, with a per-adapter default. This pairs
with the legacy `model_name(const AgentTask&)` so V1 callers remain
unchanged.

### 5.3 invoke() body

Establish the shape early:

```cpp
const auto cancelled = [&]() {
    return invocation.cancel && invocation.cancel->is_cancelled();
};
const auto elapsed_ms = [&]() {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at).count());
};

if (cancelled()) {
    return MakeCancelledResult(elapsed_ms());
}
```

Then emit `SessionInit` (if you have a session id), one or more `Status`
events for human-readable progress, the body of the call, and finally
`Usage` + `Final`. The `EmitEvent` helper is the recommended pattern: it
no-ops when `on_event` is empty, and propagates the `false` return:

```cpp
bool EmitEvent(const AgentEventCallback& on_event, AgentEvent event) {
    if (!on_event) {
        return true;
    }
    return on_event(event);
}
```

For zero-cost adapters, populate `result.usage.cost_usd = 0.0` and
`result.usage.turns = 1` explicitly so the budget gate sees data rather
than missing fields. This is what `LocalPlanningAgent` and `GeminiAgent`
do.

### 5.4 cancel(task_id) becomes a no-op stub

```cpp
bool LocalPlanningAgent::cancel(const std::string& /*task_id*/) {
    // Phase 4.1: legacy cancel(task_id) is a no-op stub. Cancellation is now
    // driven via AgentInvocation::cancel (CancellationToken). No callers in
    // the live codebase, so returning false is safe.
    return false;
}
```

The V1 `cancel(task_id)` method was the source of the per-adapter
`task_id` -> state map. With the shared `CancellationToken` it has no
remaining purpose. Leave it as a returning-`false` stub for vtable
completeness; the dynamic_cast routing in `SubagentManager` /
`AgentLoop` ensures it is never called against a migrated adapter.

### 5.5 Tests

The reference smoke test for cancellation is
`TestLocalPlanningAgentV2Cancels` in `tests/subagent_session_tests.cpp`
(it documents the stable `Cancelled` error code). Mirror the shape for
your adapter: pre-trip the token, call `invoke()`, assert the result has
`error_code == "Cancelled"`.

---

## 6. Routing semantics

Both `SubagentManager::run_one` and `AgentLoop::run_agent_task` follow the
same pattern. After resolving the agent and evaluating policy, they
`dynamic_cast<IAgentAdapterV2*>` the adapter pointer and prefer
`invoke()` when the cast succeeds:

```cpp
// Phase 4 routing: prefer the V2 invoke() path on adapters that implement
// IAgentAdapterV2 so the orchestrator gets structured AgentUsage and a
// CancellationToken hook. Legacy-only adapters fall back to run_task().
AgentResult agent_result;
if (auto* v2 = dynamic_cast<IAgentAdapterV2*>(agent.get())) {
    AgentInvocation invocation;
    invocation.task_id = agent_task.task_id;
    invocation.objective = agent_task.objective;
    invocation.workspace_path = task.workspace_path;
    invocation.context = {
        {"task_type", task.task_type},
        {"parent_task_id", task.task_id},
        {"agent", agent_name},
        {"role", role},
        {"original_objective", task.objective},
        {"subtask_objective", subtask_objective},
    };
    invocation.timeout_ms = task.timeout_ms;
    invocation.budget_limit_usd = task.budget_limit;
    // Forward the orchestrator-scoped cancel token. V2 adapters that
    // honor it can interrupt mid-call (e.g. close the SSE socket); the
    // rest still get the pre-dispatch check above.
    invocation.cancel = cancel;
    agent_result = v2->invoke(invocation);
} else {
    // Legacy path has no in-flight cancellation hook; the pre-dispatch
    // check above is the only interruption point.
    agent_result = agent->run_task(agent_task);
}
```

`AgentLoop::run_agent_task` does the same with a smaller `context` map
(`task_type`, `parent_task_id`, `agent`) and lifts
`task.inputs["model"]` into `invocation.constraints["model"]` when present.

After dispatch, both call sites pick the effective cost the same way:

```cpp
// Prefer measured `usage.cost_usd` when the V2 adapter populated it; fall
// back to legacy `estimated_cost` when usage is empty (zero means "we
// measured zero", but V2 adapters that have nothing to measure leave
// both fields at 0.0 so the orchestrator behavior is unchanged).
const double effective_cost = agent_result.usage.cost_usd > 0.0
    ? agent_result.usage.cost_usd
    : agent_result.estimated_cost;
```

This is the rule: a strictly positive `usage.cost_usd` wins; otherwise the
legacy `estimated_cost` is used. The result lands on
`TaskStepRecord::estimated_cost` and from there into
`MemoryManager::agent_stats` and the parallel-mode budget gate.

Cancellation checkpoints in the orchestrator:

- `AgentLoop::run`: pre-routing — if the token is already tripped before
  route selection, return immediately with `Cancelled`.
- `AgentLoop::run_agent_task`: pre-dispatch after policy — denial
  diagnostics still flow through audit, then the cancellation short-circuit
  records a `Cancelled` `TaskStepRecord` and returns.
- `SubagentManager::run_one`: pre-dispatch before agent lookup, and again
  after policy evaluation for the legacy `run_task` path.
- `SubagentManager::run` (sequential mode): observed before each agent
  dispatch — remaining agents record a `Cancelled` step and are skipped.
  Parallel mode launches all futures first, so cancellation is observed
  inside each `run_one`.

---

## 7. Known gaps

- **End-to-end streaming coverage is partial.** `CodexCliAgent`,
  `QwenAgent`, and `AnthropicAgent` parse upstream streams and emit the
  full event vocabulary. `LocalPlanningAgent` and `GeminiAgent` are sync
  wraps that emit a small canned event sequence around the legacy
  `run_task` path; they do not interrupt mid-call.
- **Wire protocols are per-adapter.** Codex uses NDJSON, Qwen and
  Anthropic use SSE with different framing, Gemini's REST plumbing is
  request/response. There is no shared SSE/NDJSON parser yet — each
  adapter rolls its own. Consolidating into a small streaming utilities
  module is a deferred follow-up.
- **`CompactBoundary` is unused.** No current adapter surfaces context
  compaction events. The `Kind` is reserved for future Anthropic / Codex
  releases that expose the boundary explicitly.
- **`from_stream_fallback` is set but rarely consumed.** Today only
  `AnthropicAgent` flips it on a streaming early-failure retry. The
  orchestrator does not yet penalize adapters with a high fallback rate —
  the field is plumbed for the future budget / health logic.
- **`IAgentAdapter` is not going away soon.** The dual-inheritance pattern
  is the deliberate interim: legacy callers (`AgentRegistry::list_profiles`,
  the V1 unit tests, plugin shims) still depend on the V1 vtable. Removal
  is gated on every caller and test migrating to a V2 entry point. Do not
  add new V1-only adapters; do not delete V1 yet.
