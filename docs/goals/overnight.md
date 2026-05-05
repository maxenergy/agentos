# Overnight Goal Runner

Use this file when asking Codex `/goal` to work unattended for a long stretch.

Recommended invocation:

```text
/goal Work through docs/goals/overnight.md until all unblocked goals are complete or a stop condition is reached. Commit each completed goal separately.
```

## Objective

Complete as many ready goals from `docs/goals/backlog.md` as possible, in dependency order, without mixing unrelated worktree changes and without forcing blocked goals.

## Required Reading

Before changing code, read:

- `CONTEXT.md`
- `improve-plan.md`
- `docs/goals/README.md`
- `docs/goals/backlog.md`
- `docs/goals/verification.md`
- the specific `docs/goals/G*.md` file for the current goal

## Execution Rules

- Work one goal at a time.
- Start with ready goals only.
- Do not attempt goals marked blocked unless their dependency was completed earlier in this same run.
- Do not attempt `G016-provider-oauth-watch.md` unless stable public Anthropic PKCE endpoints are verified from official provider documentation.
- Commit each completed goal separately.
- Use a Conventional Commit message that names the goal, for example:
  - `feat(agent-dispatch): add dispatch seam`
  - `refactor(auth): extract login flow modules`
  - `docs(goals): update goal backlog`
- After each completed goal:
  - update the goal file status to `done`;
  - update `docs/goals/backlog.md`;
  - add a short note to `docs/goals/decision-log.md` if a boundary was clarified;
  - run the verification listed in the goal file;
  - commit only files touched for that goal.
- If a goal cannot be completed, stop and write a blocker note in `docs/goals/decision-log.md`.

## Stop Conditions

Stop immediately when any of these happens:

- A test failure cannot be fixed within the current goal scope.
- The work requires reverting or overwriting unrelated local changes.
- A goal requires a decision not already resolved in `CONTEXT.md`, `improve-plan.md`, or its goal file.
- A goal requires a new third-party dependency.
- A goal requires changing public behavior beyond its acceptance criteria.
- Git cannot cleanly stage only the current goal's files.
- You reach `G016-provider-oauth-watch.md` and stable provider endpoints are not officially available.

## Recommended Order

Batch 1:

1. `G001-agent-dispatch-seam.md`
2. `G005-capability-contract-facade.md`
3. `G011-runtime-store-backend-seam.md`
4. `G014-auth-login-flow-modules.md`

Batch 2:

1. `G002-agent-dispatch-callers.md`
2. `G006-contract-callers.md`
3. `G012-storage-commands-backend.md`
4. `G015-auth-profile-strategy.md`

Batch 3:

1. `G003-decomposition-dispatch.md`
2. `G004-repl-dispatch.md`
3. `G007-learn-skill-capability-declaration.md`
4. `G008-route-hints.md`
5. `G009-plugin-protocol-output.md`
6. `G013-audit-history-recovery.md`

Batch 4:

1. `G010-plugin-session-policy.md`
2. `G017-domain-docs-maintenance.md`

External watch:

- `G016-provider-oauth-watch.md`

## Morning Report

Before stopping, write a short report in `docs/goals/overnight-report.md` with:

- completed goals;
- commits created;
- verification commands run;
- failed or skipped goals with reasons;
- current recommended next goal;
- any files intentionally left modified.

