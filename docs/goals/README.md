# AgentOS Goal Packets

This directory contains small, executable goal packets for Codex `/goal` work.
Use these files instead of giving `/goal` the whole `improve-plan.md`.

## Operating Rules

- Read `CONTEXT.md` before implementing any goal.
- Work on one goal file at a time.
- Do not stage or revert unrelated worktree changes.
- Keep changes inside the goal scope unless the codebase proves a narrow extra file is required.
- Preserve existing behavior unless the goal explicitly changes it.
- Update the goal file status only when the acceptance criteria are actually met.
- Record new durable terminology in `CONTEXT.md` as soon as it is resolved.
- Create an ADR only when the decision is hard to reverse, surprising without context, and a real trade-off.
- Run the focused verification listed in the goal file. Run full verification when the goal touches shared behavior.

## How To Invoke

Use one goal packet directly:

```text
/goal Complete docs/goals/G001-agent-dispatch-seam.md end to end.
```

When a goal finishes:

- Update `docs/goals/backlog.md`.
- Add a short note to `docs/goals/decision-log.md` if the work clarified a design boundary.
- Update `improve-plan.md` only when the goal changes the overall architecture plan.

## Goal Order

Start with the ready goals in `docs/goals/backlog.md`. The recommended first goal is:

- `G001-agent-dispatch-seam.md`

