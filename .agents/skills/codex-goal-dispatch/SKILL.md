---
name: codex-goal-dispatch
description: Dispatch a compiled /goal objective to any AgentOS coding agent (codex_cli, anthropic, gemini, qwen). Handles per-task verification, retry-with-error-context feedback loop, scope checks, budget-limit detection, and adversarial Stop Rule review. Use after /goal-compiler has produced docs/goal/GOAL.md.
---

# Codex Goal Dispatch (AgentOS)

This is the AgentOS-specific version of `/goal-dispatch`. It is identical in behavior to the general `/goal-dispatch` skill from the skills plugin, and exists here for use when Claude Code is working within the AgentOS project itself.

## Agent selection

AgentOS provides these coding agent adapters. Ask the user which to use if not specified:

| Agent key | When to prefer |
|-----------|----------------|
| `codex_cli` | OpenAI Codex CLI -- best for /goal integration with its built-in continuation loop |
| `anthropic` | Claude via Anthropic API -- strong reasoning, no /goal state machine |
| `gemini` | Gemini via Google API |
| `qwen` | Qwen via Alibaba API |

Default to `codex_cli` when the goal was compiled by `/goal-compiler` (because Codex's internal continuation.md protocol aligns with the goal format). Use `anthropic` for tasks that need heavy reasoning or where Codex's quota is exhausted.

## Prerequisites

Verify before dispatching:

- `docs/goal/GOAL.md` exists
- `docs/goal/TASKS.md` exists
- `docs/goal/ACCEPTANCE.md` exists
- `build/agentos` is built: `cmake --build build` if stale
- `git status --short` returns nothing, or only untracked files under `docs/goal/`

## Initial dispatch

```
agentos run <agent_key> objective="$(cat docs/goal/GOAL.md)"
```

For AgentOS development tasks specifically, you can also pass `allow_high_risk=true` if the task requires file writes or shell execution that the policy engine would otherwise block.

## Verify loop

After each agent run, verify the tasks completed in that run:

1. Run the Verification command from `docs/goal/TASKS.md` for the current task.
2. **Pass**: log `[PASS] Task N` in `docs/goal/DISPATCH_LOG.md`, continue.
3. **Fail**: build retry objective (see below) and rerun.

For AgentOS C++ tasks, the standard verification sequence is:

```
cmake --build build
ctest --test-dir build -R <relevant_test> --output-on-failure --timeout 60
```

Full suite check (run before final review):

```
cmake --build build
ctest --test-dir build --output-on-failure --timeout 60
```

## Retry protocol

On verification failure, build a retry objective that embeds the accumulated error context:

```
<original docs/goal/GOAL.md content>

---
## Retry context

Verification failed for Task N: <name>

Command: `<command>`
Exit code: <n>
Output:
<truncated to 2000 chars>

Fix this failure before continuing. Do not mark any task complete until
its verification command passes.
```

Rerun with this objective. Retry up to 2 times per task. On the third consecutive failure, pause and present to the user.

## Safety -- scope check

After each agent run, run `git diff --name-only`. If files outside the task's Scope appear:

1. Log the unexpected files.
2. Ask: "Agent touched files outside scope: `<files>`. Continue, revert those files, or stop?"

Never revert automatically. This is especially important for AgentOS: the agent might accidentally touch `runtime/` or `src/` files not in scope.

## Final review

When all task verifications pass:

1. Run every condition in the Stop Rule section of `docs/goal/ACCEPTANCE.md`.
2. Run the full test suite: `ctest --test-dir build --output-on-failure --timeout 60`.
3. Run `git diff --name-only` against the expected file set.
4. **All pass**: report complete, update `docs/goal/DISPATCH_LOG.md`.
5. **Any fail**: rerun with a final-review retry objective that lists the failing conditions.

## Parallel agent orchestration

For independent tasks (Dependencies: none), AgentOS can run multiple agents in parallel via `SubagentManager`. This is advanced usage -- only apply it when tasks are explicitly marked independent in `docs/goal/TASKS.md`.

To run two tasks in parallel:

```
agentos run anthropic objective="Task A: <goal>" &
agentos run anthropic objective="Task B: <goal>" &
wait
```

Merge their outputs manually and run the combined verification before proceeding.

## Dispatch log

Maintain `docs/goal/DISPATCH_LOG.md`:

```
## Dispatch Log

Agent: <agent_key>
Project: AgentOS

### Task N: <name>
- Status: pass / fail / retrying
- Verification: `<command>` -> exit <n>
- Scope: ok / unexpected: <files>
- Attempts: <n>

### Final Status
- Tasks: N/N passed
- Stop Rule: pass / fail
- Full test suite: pass / fail
```
