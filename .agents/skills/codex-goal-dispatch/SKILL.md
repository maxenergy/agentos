---
name: codex-goal-dispatch
description: Dispatch a compiled /goal objective to the codex_cli subagent via AgentOS, run per-task verification after each Codex turn, feed failures back to the same thread, and handle budget-limit states. Use after /goal-compiler has produced docs/goal/GOAL.md.
---

# Codex Goal Dispatch

Dispatch the compiled goal to Codex via AgentOS's `codex_cli` adapter and run the verify-then-feedback loop until the goal is achieved, budget-limited, or blocked on a human decision. This skill is the AgentOS side of the pipeline — it assumes `/goal-compiler` has already produced `docs/goal/GOAL.md`.

## Prerequisites

Before dispatching, verify all of the following. Stop and report which is missing if any fails:

- `docs/goal/GOAL.md` exists
- `docs/goal/TASKS.md` exists
- `docs/goal/ACCEPTANCE.md` exists
- `git status --short` returns nothing, or only untracked files under `docs/goal/`

## Initial dispatch

Run the `codex_cli` subagent with the goal as its objective:

```
agentos run codex_cli objective="$(cat docs/goal/GOAL.md)"
```

Use a single persistent thread. Do not restart the thread unless the user explicitly asks. Codex's goal state machine (`pursuing → achieved / unmet / budget-limited`) must run to completion in one thread.

## Verify loop

After each Codex response, run the verification commands for the tasks Codex worked on:

1. Run the `Verification` command for the task just completed.
2. **Pass**: log `[PASS] Task N: <name>` in `docs/goal/DISPATCH_LOG.md` and continue.
3. **Fail**: capture the exact command, exit code, stdout, and stderr (cap at 2000 chars). Send this back to the same Codex thread:

```
Verification failed for Task N: <name>

Command: <exact command>
Exit code: <n>
Output:
<stderr/stdout, capped at 2000 chars>

Please diagnose and fix before continuing.
```

4. Retry up to two times for the same task. On the third consecutive failure, pause and present the failure to the user:
   - What command failed
   - The full output
   - The question: "Should I continue retrying, adjust the verification command, or stop?"

## Safety — scope check

Before each Codex continuation turn, run `git diff --name-only`. Check that every changed file appears in the current task's Scope field in `docs/goal/TASKS.md`. If unexpected files appear:

1. Log the unexpected files in `docs/goal/DISPATCH_LOG.md`.
2. Ask the user: "Codex modified files outside the task scope: `<files>`. Continue, roll back those files, or stop?"

Never roll back automatically.

## Budget-limit handling

If Codex's response contains language consistent with `budget_limit.md` injection (summaries of progress, "remaining work includes…", no new code changes):

1. Stop the dispatch loop.
2. Present the user with:
   - Tasks completed (from `docs/goal/DISPATCH_LOG.md`)
   - Tasks remaining (from `docs/goal/TASKS.md`)
   - Codex's exact summary of the blocker or remaining work
   - The next step recommendation: "Resume with `/codex-goal-dispatch` after adjusting budget, or split remaining tasks."

Do not automatically resume. Ask the user.

## Final review

When Codex marks the goal `achieved`:

1. Run every criterion in the Stop Rule section of `docs/goal/ACCEPTANCE.md`.
2. Run `git diff --name-only` and verify it matches the expected file set.
3. **All pass**: report "Goal achieved. All acceptance criteria verified." Update `docs/goal/DISPATCH_LOG.md` with the final status.
4. **Any fail**: do not accept the completion. Send back to the same Codex thread:

```
The goal was marked achieved prematurely. The following acceptance criteria did not pass:

<list of failed criteria with commands and outputs>

Please fix these before calling update_goal with status=achieved.
```

## Dispatch log format

Maintain `docs/goal/DISPATCH_LOG.md` throughout:

```markdown
## Dispatch Log

### Task N: <name>
- Status: pass / fail / retrying / pending
- Verification: `<command>` → exit <n>
- Files changed: <git diff --name-only output>
- Notes: <any anomalies>

### Final Status
- Goal state: achieved / budget-limited / unmet / in-progress
- Criteria verified: N/N
```
