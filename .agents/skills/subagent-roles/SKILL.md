---
name: subagent-roles
description: Assign specialized roles to AgentOS subagents for a multi-agent development run. Use when a goal requires simultaneous work across independent concerns (e.g., backend + frontend, tests + implementation, docs + code) and you want to split it across codex_cli, anthropic, gemini, or qwen in parallel.
---

# Subagent Roles

Decompose a compiled goal into parallel workstreams and assign each to the most appropriate AgentOS agent adapter. Use after `/task-decomposer` has identified which tasks are independent (Dependencies: none).

## When to use this instead of /goal-dispatch

Use `/goal-dispatch` for sequential tasks where each depends on the previous output.

Use `/subagent-roles` when:
- Two or more tasks in `docs/goal/TASKS.md` have `Dependencies: none`
- Those tasks touch non-overlapping files (confirmed by their Scope fields)
- Running them in parallel would meaningfully reduce wall-clock time

Do NOT parallelize tasks that share files -- the merge conflict cost exceeds the time saving.

## Agent role assignment

Match tasks to agents by strength:

| Workstream type | Preferred agent |
|----------------|----------------|
| Implementation with /goal state machine | `codex_cli` |
| Architecture decisions, complex reasoning | `anthropic` |
| Large context, long file analysis | `gemini` |
| Cost-sensitive bulk work | `qwen` |

If the user has not specified, assign `codex_cli` to the implementation task and `anthropic` to any design/analysis task.

## Pre-flight checklist

Before parallelizing:

1. Confirm independent tasks have non-overlapping Scope fields.
2. Confirm `git status --short` is clean.
3. Create a separate git stash or worktree per parallel task if available.
4. Read `docs/goal/GOAL.md` aloud to the user and ask: "I'll split tasks [N, M] to run in parallel with [agent A, agent B]. Confirm?"

## Dispatch pattern

For each independent task group:

```
# Build a focused objective for each agent
TASK_A_OBJECTIVE="$(cat docs/goal/GOAL.md)

---
## Focused scope for this agent

You are responsible for Task N only: <task name>
Scope: <task scope>
Verification: `<task verification command>`
Do not touch: <do not touch list>"

TASK_B_OBJECTIVE="..."

# Run in parallel
agentos run codex_cli objective="$TASK_A_OBJECTIVE" &
PID_A=$!
agentos run anthropic objective="$TASK_B_OBJECTIVE" &
PID_B=$!
wait $PID_A $PID_B
```

## Merge and verify

After both complete:

1. Run `git diff --name-only` and confirm the union of both scope lists.
2. Run each task's Verification command independently.
3. If either fails, retry only the failed task (do not rerun the passing one).
4. Run the full test suite once both tasks pass their individual verifications.

## Conflict resolution

If both agents modified the same file:

1. Stop. Do not attempt an automated merge.
2. Present the conflict to the user: which file, which task touched it, what each change does.
3. Ask the user to resolve the conflict manually, or assign one agent to redo the conflicting part sequentially.
