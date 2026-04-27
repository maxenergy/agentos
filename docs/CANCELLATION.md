# Cancellation

## 1. Overview

AgentOS supports end-to-end cooperative cancellation for long-running
agent work. A single `CancellationToken` instance is owned by the
orchestrator, tripped by the CLI's signal handler, and observed by
`AgentLoop`, `SubagentManager`, and every V2 agent adapter. Cooperative
is deliberate: the kernel raises a flag and lets each component unwind
cleanly so audit, memory, and budget accounting stay coherent. A tripped
token causes the orchestrator to fail-fast at every checkpoint with a
stable `error_code = "Cancelled"`, V2 adapters interrupt between
upstream calls or in their stream-consumer loops, and the user always
retains a hard escape via a second Ctrl-C / SIGINT that bypasses the
cooperative path entirely.

---

## 2. CancellationToken reference

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

Method contracts (from `src/utils/cancellation.cpp`):

- `cancel()` is `noexcept` and idempotent. It takes the mutex, stores
  `true` with release semantics, and calls `cv_.notify_all()` so every
  waiter on `wait_for_cancel` wakes. Repeated calls are safe.

- `is_cancelled()` is `noexcept` and a single atomic acquire load.
  Safe to call from any thread.

- `wait_for_cancel(timeout)` takes a `std::unique_lock` and returns
  `cv_.wait_for(lock, timeout, predicate)`. The predicate is the same
  atomic acquire-load, so it returns `true` if cancelled before timeout
  and `false` on timeout. Use this instead of
  `std::this_thread::sleep_for` so a sleep unblocks immediately on trip.

`notify_all` (rather than `notify_one`) is deliberate. Multiple
subsystems may hold the same token concurrently — parallel-mode
`SubagentManager` launches every `run_one` future before observing
cancellation, and each adapter inside those futures may have its own
watchdog thread polling the token.
`TestWaitForCancelReleasesAllWaiters` in `tests/cancellation_tests.cpp`
pins this behavior: four concurrent waiters all observe the trip after a
single `cancel()` call.

---

## 3. InstallSignalCancellation reference

Defined in `src/utils/signal_cancellation.hpp`:

```cpp
// Installs a process-wide Ctrl-C / SIGINT (and SIGTERM on POSIX) handler that
// trips the returned CancellationToken. Idempotent — subsequent calls return
// the same token and do not re-install the handler.
//
// First signal: token is cancelled, the handler returns, and any in-flight
// orchestration that holds the token cooperatively unwinds with `Cancelled`.
// Second signal: the handler restores the OS default disposition and re-raises
// the signal, so the user can still hard-kill if cooperative cancel is too
// slow. Stderr gets a one-line hint after the first signal.
//
// The token outlives the process (it is owned by a function-local static), so
// callers can safely capture it by `std::shared_ptr<CancellationToken>` and
// keep it past the lifetime of any single command handler.
std::shared_ptr<CancellationToken> InstallSignalCancellation();
```

Implementation notes from `src/utils/signal_cancellation.cpp`:

- A function-local static `SignalState` owns the
  `std::shared_ptr<CancellationToken>`, an `std::atomic<bool>
  handler_installed`, and an `std::atomic<int> signal_count`. The token
  outlives every command handler.

- Idempotence is enforced with a CAS on `handler_installed`. The first
  successful CAS installs the OS handler; every subsequent call returns
  the already-stored token. `TestSignalCancellationInstallReturnsConsistentToken`
  asserts two installs return the same `get()`.

- First-signal hint is printed via `NoteFirstSignal()`. On Windows the
  handler runs in a separate thread, so `std::fputs` + `fflush` is
  acceptable. On POSIX the handler runs on the signal stack, so the
  function uses `::write(2, ...)` (the only signal-safe way to reach
  stderr — `fputs` is not async-signal-safe).

- The two-signal contract is encoded in `signal_count.fetch_add(1)`'s
  return value (`previous`):

  - **Windows** (`ConsoleCtrlHandler`): handles
    `CTRL_C_EVENT`, `CTRL_BREAK_EVENT`, and `CTRL_CLOSE_EVENT`. First
    signal calls `state.token->cancel()`, prints the hint, and returns
    `TRUE` so Windows treats the signal as consumed and the process keeps
    running to observe the cooperative cancel. Second-and-later signals
    return `FALSE`, which lets Windows fall through to the default
    disposition (process termination).

  - **POSIX** (`PosixSignalHandler`): registered for `SIGINT` and
    `SIGTERM` via `sigaction` with `SA_RESTART`. First signal calls
    `cancel()` and prints the hint via the signal-safe `write(2)` path.
    Second-and-later signals install
    `default_action.sa_handler = SIG_DFL` for the originating signal and
    call `raise(signum)` so the process dies promptly with the OS
    default disposition.

- If `SetConsoleCtrlHandler` fails on Windows, the install resets
  `handler_installed` and still returns the token. Callers always get a
  usable object even if the OS handler refused to install.

---

## 4. Propagation diagram

```
   OS signal (Ctrl-C / SIGINT / SIGTERM / Ctrl-Break / Ctrl-Close)
            |
            v
   InstallSignalCancellation() handler
   - first signal:  token.cancel(); swallow / return
   - second signal: restore OS default; terminate process
            |
            v
   shared_ptr<CancellationToken> (captured at CLI dispatch)
            |
   +--------+--------+
   |                 |
   v                 v
 AgentLoop::run    SubagentManager::run
 - pre-route       - sequential: short-circuit remaining
   fast-fail         agents -> Cancelled steps
 - run_agent_task  - parallel:   each future passes the
   pre-dispatch     same token into run_one
   fast-fail
            |
            v
 AgentInvocation.cancel  (V2 dispatch path only)
            |
            v
 V2 adapter checkpoints:
   - entry  ->  Cancelled before request
   - before each upstream API call
   - inside SSE / NDJSON chunk handlers
   - watchdog loops: wait_for_cancel(timeout)
   -> AgentResult{ success=false, error_code="Cancelled" }
```

---

## 5. CLI driver wiring

Both interactive entry points install the same handler and pass the
returned token straight into the orchestrator. The shape is identical:
install, pass through, return.

`src/main.cpp` (the `agentos run <skill> ...` dispatch):

```cpp
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
```

`src/cli/subagents_commands.cpp` (the `agentos subagents run` dispatch):

```cpp
if (command == "run") {
    const auto agent_names = options.contains("agents")
        ? SplitCommaList(options.at("agents"))
        : std::vector<std::string>{};
    const auto mode = ParseSubagentExecutionMode(options.contains("mode") ? options.at("mode") : "sequential");
    const auto task = BuildSubagentTaskFromOptions(options, workspace);
    // Bind a process-wide Ctrl-C / SIGINT handler so the orchestrator can
    // unwind in-flight V2 invocations cooperatively. Second signal hard-
    // kills (handler restores OS default disposition and re-raises).
    auto cancel = InstallSignalCancellation();
    const auto result = subagent_manager.run(task, agent_names, mode, std::move(cancel));
    PrintResult(result);
    return result.success ? 0 : 1;
}
```

There are exactly two `InstallSignalCancellation()` call sites in the
codebase today; the third caller (the unit test) installs solely to
assert idempotence. Skill-routed `agentos run <skill> ...` invocations
still install the handler but the skill execution path does not
propagate the token (see §10).

---

## 6. Orchestrator behavior

### AgentLoop::run

`src/core/loop/agent_loop.cpp` checks the token twice. First, before
routing — so a token that is already tripped at dispatch never spends
cycles on route selection, cache lookup, or policy evaluation:

```cpp
TaskRunResult AgentLoop::run(const TaskRequest& task, std::shared_ptr<CancellationToken> cancel) {
    audit_logger_.record_task_start(task);

    // Pre-routing check: if the orchestrator cancelled before we even
    // selected a target, fail fast with Cancelled rather than spending time
    // on routing / cache / policy.
    if (cancel && cancel->is_cancelled()) {
        TaskRunResult cancelled;
        cancelled.success = false;
        cancelled.summary = "Task cancelled before routing.";
        cancelled.error_code = "Cancelled";
        cancelled.error_message = "AgentLoop observed a tripped cancellation token before route selection.";
        FinalizeTaskRun(audit_logger_, memory_manager_, task, cancelled);
        return cancelled;
    }
```

Second, inside `run_agent_task`, after policy evaluation but before
dispatch. The order matters: policy denials still have to run so the
audit log records why a denied task was rejected, but a tripped token
short-circuits before any agent code runs:

```cpp
// Pre-dispatch cancellation check after policy so denial diagnostics
// still flow through audit even when the user is also cancelling.
if (cancel && cancel->is_cancelled()) {
    TaskStepRecord cancelled_step{
        .target_kind = RouteTargetKind::agent,
        .target_name = route.target_name,
        .success = false,
        .duration_ms = ElapsedMs(started_at),
        .error_code = "Cancelled",
        .error_message = "agent dispatch was cancelled by the orchestrator",
    };
    RecordTaskStep(audit_logger_, task.task_id, cancelled_step);
    return {
        .success = false,
        .summary = "Agent dispatch cancelled.",
        .route_target = route.target_name,
        .route_kind = RouteTargetKind::agent,
        .error_code = "Cancelled",
        .error_message = cancelled_step.error_message,
        .duration_ms = cancelled_step.duration_ms,
        .steps = {cancelled_step},
    };
}
```

If neither check trips and the resolved adapter implements
`IAgentAdapterV2`, the loop forwards the same shared pointer through
`invocation.cancel = cancel;` so the adapter can poll mid-call.

### SubagentManager::run

`src/core/orchestration/subagent_manager.cpp` accepts the token by
default-null `std::shared_ptr<CancellationToken>` so legacy call sites
keep compiling unchanged. Its sequential and parallel modes have
distinct behavior.

Sequential mode short-circuits remaining dispatches once tripped, but
records a `Cancelled` `TaskStepRecord` for every skipped agent so the
audit / memory layers still see a row per requested agent:

```cpp
} else {
    for (std::size_t index = 0; index < normalized_agent_names.size(); ++index) {
        // Sequential mode short-circuits as soon as the orchestrator
        // cancels: subsequent agents record a Cancelled step instead of
        // dispatching. (Parallel mode already started every future before
        // we observe the cancel.)
        if (cancel && cancel->is_cancelled()) {
            result.steps.push_back(TaskStepRecord{
                .target_kind = RouteTargetKind::agent,
                .target_name = normalized_agent_names[index],
                .success = false,
                .duration_ms = 0,
                .error_code = "Cancelled",
                .error_message = "subagent dispatch was cancelled by the orchestrator",
            });
            continue;
        }
        result.steps.push_back(run_one(effective_task, normalized_agent_names[index], subagent_roles[index], cancel));
    }
}
```

Parallel mode dispatches every future before observing cancellation, so
the token must reach `run_one`. The `std::async` lambda captures the
shared pointer by value, and `run_one` checks the token in its prologue:

```cpp
futures.push_back(std::async(std::launch::async, [this, &effective_task, agent_name, role, cancel]() {
    return run_one(effective_task, agent_name, role, cancel);
}));
```

```cpp
// Pre-dispatch cancellation: covers parallel futures that the executor
// launched before the orchestrator tripped the token. The check repeats
// after policy evaluation below for the legacy run_task path.
if (cancel && cancel->is_cancelled()) {
    return {
        .target_kind = RouteTargetKind::agent,
        .target_name = agent_name,
        .success = false,
        .duration_ms = ElapsedMs(started_at),
        .error_code = "Cancelled",
        .error_message = "subagent dispatch was cancelled by the orchestrator",
    };
}
```

The same `invocation.cancel = cancel;` forward applies on the V2 path.
Legacy `run_task()` adapters get the pre-dispatch check and nothing more.

---

## 7. V2 adapter contract

Every V2 adapter (`IAgentAdapterV2::invoke`) receives the same shared
pointer in `invocation.cancel`. To honor cancellation an adapter MUST:

- Check `invocation.cancel && invocation.cancel->is_cancelled()` at every
  natural break point: on entry, before each upstream API call, after
  each parsed event, between retries.
- Return an `AgentResult` with `success = false` and
  `error_code = "Cancelled"` when it observes the trip. The text of
  `error_message` is adapter-specific (e.g. `"local planning task was
  cancelled"`, `"qwen invoke cancelled"`, `"gemini invocation was
  cancelled before dispatch"`); only the error code is normative.

It MAY:

- Use `wait_for_cancel(timeout)` from `CancellationToken` instead of
  `std::this_thread::sleep_for(timeout)` so a sleep unblocks immediately
  on trip.
- Tear down an in-flight HTTP read or process pipe when cancelled, if the
  protocol allows it. The streaming adapters do this — they kill the
  child curl / codex process via `TerminateJobObject` (Windows) or signal
  delivery (POSIX) once the watchdog observes the trip.
- Treat a `false` return from the `AgentEventCallback` as an additional
  cancel signal and behave the same way (this is the orchestrator's path
  for budget / mid-stream policy denials).

The two implementation patterns in the migrated adapters are:

**Streaming with watchdog polling.** `CodexCliAgent`, `AnthropicAgent`,
and `QwenAgent` spawn a child process (or hold an SSE socket) and run a
loop that peeks the pipe / select-waits on the socket, polling the
cancellation token between reads. From `qwen_agent.cpp`:

```cpp
// Read in a non-blocking-ish loop: peek before ReadFile so we can poll
// cancellation/timeout/process exit without hanging on a long-poll SSE.
std::string buffer;
bool exit_loop = false;
bool aborted_for_cancel_or_callback = false;
while (!exit_loop) {
    if (cancel && cancel->is_cancelled()) {
        result.cancelled = true;
        aborted_for_cancel_or_callback = true;
        break;
    }
```

`codex_cli_agent.cpp` uses the same shape — a top-level `while (true)`
loop with a `cancel && cancel->is_cancelled()` check before each pipe
peek, terminating the job object when the trip is observed.
`anthropic_agent.cpp` polls between `WaitForSingleObject` returns. The
final `AgentResult` is normalized at one site:

```cpp
if (stream_outcome.cancelled || cancelled()) {
    // ...
    ev.fields["error_code"] = kCancelledErrorCode;
    ev.fields["error_message"] = "qwen invoke cancelled";
```

**Synchronous wrap.** `LocalPlanningAgent` and `GeminiAgent` are not
streaming today; they check the token at the entry point and at one or
two safe interior points, then run their (legacy) body to completion.
From `local_planning_agent.cpp`:

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

`gemini_agent.cpp` does the same at dispatch entry:

```cpp
if (invocation.cancel && invocation.cancel->is_cancelled()) {
    return {
        .success = false,
        .duration_ms = elapsed_ms(),
        .error_code = "Cancelled",
        .error_message = "gemini invocation was cancelled before dispatch",
    };
}
```

These wraps cannot interrupt a request that is already in flight — the
cancellation only fires at the next checkpoint. Migrating one of them to
streaming gives mid-call interruption automatically.

---

## 8. Two-signal escape hatch

User-visible behavior:

1. **First Ctrl-C** (or `SIGINT` / `SIGTERM` / `Ctrl-Break` /
   `Ctrl-Close`): the kernel cooperatively cancels. The orchestrator's
   pre-route, pre-dispatch, and adapter checkpoints all return
   `Cancelled`. Audit log entries are written; memory stats roll up the
   `Cancelled` step. Stderr prints:

   ```
   agentos: cancellation requested; press Ctrl-C again to force exit.
   ```

2. **Second Ctrl-C** (or any subsequent signal): the cooperative path is
   bypassed and the OS terminates the process. On Windows, the handler
   returns `FALSE` and Windows applies the default Ctrl-C disposition.
   On POSIX, the handler restores `SIG_DFL` for the originating signum
   and `raise(signum)` so the process dies promptly. No audit /
   memory teardown is guaranteed past this point — the next start-up
   replays from the on-disk state.

This matters because cooperative cancellation can stall on a misbehaving
upstream provider. A V2 adapter that fails to poll the token while
holding a long-poll HTTP socket open will not unwind, no matter how many
times the user types Ctrl-C cooperatively. The escape hatch guarantees
the user always retains a real exit. Implementation lives in
`src/utils/signal_cancellation.cpp` (`ConsoleCtrlHandler` on Windows,
`PosixSignalHandler` on POSIX); the OS-level fallback is the entire
reason that file uses `signal_count.fetch_add` instead of just calling
`cancel()` and returning.

---

## 9. Testing patterns

`tests/cancellation_tests.cpp` is the unit-level reference for the
primitives. The shapes worth copying:

- **Pre-trip then probe.** Construct a `CancellationToken`, call
  `cancel()`, assert `is_cancelled()`. Call `cancel()` again to confirm
  idempotence. (`TestCancellationTokenIsIdempotent`.)
- **Sleep equivalence.** Call `wait_for_cancel(50ms)` on an untripped
  token and assert it returns `false` after roughly 50 ms; on an
  already-cancelled token assert it returns `true` in well under 100 ms.
  (`TestWaitForCancelTimesOutWhenNotTripped`,
  `TestWaitForCancelReturnsImmediatelyWhenAlreadyCancelled`.)
- **notify_all coverage.** Spawn N waiter threads, sleep briefly so
  they all enter `wait_for_cancel`, call `cancel()`, join, assert all N
  observed the trip. (`TestWaitForCancelReleasesAllWaiters`.)
- **Install idempotence.** Call `InstallSignalCancellation()` twice and
  assert the two `shared_ptr<CancellationToken>` have the same `get()`.
  Do not call `cancel()` on the returned token in test code — it is
  process-wide and shared with sibling tests.

`tests/subagent_session_tests.cpp` covers the integration paths. The
canonical V2-adapter cancel test is `TestLocalPlanningAgentV2Cancels`:

```cpp
agentos::LocalPlanningAgent agent;
auto cancel = std::make_shared<agentos::CancellationToken>();
// Pre-cancel: the deterministic offline planner finishes too fast for a
// racing thread to win reliably, so we assert the cancel-before-invoke
// path returns the documented "Cancelled" code.
cancel->cancel();

const agentos::AgentInvocation invocation{
    .task_id = "v2-cancel",
    .objective = "ensure cancellation is honored",
    .workspace_path = isolated_workspace,
    .context = {{"task_type", "analysis"}},
    .cancel = cancel,
};

const auto result = agent.invoke(invocation);

Expect(!result.success, "cancelled V2 invoke should not report success");
Expect(result.error_code == "Cancelled", "cancelled V2 invoke should set the documented Cancelled error code");
```

The recommended adapter test shape:

1. Construct a `std::make_shared<CancellationToken>()` and call
   `cancel()` on it before dispatch.
2. Build an `AgentInvocation` with `.cancel = cancel`.
3. Call `adapter.invoke(invocation)`.
4. Assert `!result.success`, `result.error_code == "Cancelled"`, and (if
   the adapter would otherwise issue a network call) that no upstream
   request was made — fixture mocks count the requests.
5. Assert prompt return (under 1 s) so a regression that drops the entry
   check produces a flake-free failure.

The integration tests for `AgentLoop` and `SubagentManager`
(sequential and parallel) live in the same file and follow the same
pattern: pre-trip the token, run the loop / manager, assert every step
record has `error_code == "Cancelled"`. `tests/agent_provider_tests.cpp`
adds `TestCodexCliAgentV2Cancellation` and
`TestGeminiAgentV2InvokeCancelsBeforeDispatch` for the streaming and
sync-wrap V2 adapters respectively.

---

## 10. Known gaps

Cancellation does not currently cover:

- **Legacy `IAgentAdapter`-only adapters.** Adapters that have not
  migrated to `IAgentAdapterV2` get only the pre-dispatch checkpoints in
  `AgentLoop::run_agent_task` and `SubagentManager::run_one`. Once a
  legacy adapter's `run_task()` is in flight, there is no in-call
  cancellation hook — the V1 `cancel(task_id)` method was removed in
  favor of the shared token. Migrated adapters keep `cancel(task_id)` as
  a no-op stub for vtable completeness.

- **Skill-routed runs.** `AgentLoop::run` only forwards the token into
  `run_agent_task`. The `run_skill_task` branch (builtin skills, CLI
  skills, plugin skills) does not currently propagate the token to the
  skill execution path. The pre-route check still trips a tripped token
  before dispatch, but a long-running plugin or CLI skill that has
  already started will finish under the OS-default escape hatch only.

- **Scheduler daemon mode.** `agentos schedule daemon ...` runs the
  scheduler loop without calling `InstallSignalCancellation()`. Verified
  against `src/cli/schedule_commands.cpp` (the daemon dispatch at line
  480) and `src/scheduler/scheduler.cpp` (no signal-handler install or
  cancellation token plumbing). The daemon itself terminates on the
  first Ctrl-C via the OS default disposition because no handler is
  installed; in-flight scheduler-driven tasks therefore do not get the
  cooperative pre-dispatch checkpoint that interactive `agentos run`
  invocations enjoy.

- **`CompactBoundary` and other future events.** The cancellation
  contract is orthogonal to streaming event vocabulary. No current adapter
  surfaces compaction as a cancellation observation point; that is a
  natural future checkpoint when the upstream protocols start exposing
  the boundary explicitly.
