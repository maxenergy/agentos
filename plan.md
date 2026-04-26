# AgentOS Completion Review And Plan

Last updated: 2026-04-23

This file is the working plan for aligning the implementation with the docs. Update it whenever a task is completed, deferred, or re-scoped.

## Update Rules

- Mark completed tasks with `[x]` in the same commit/change set that implements them.
- Add a short note under "Progress Log" for every completed milestone.
- Keep `docs/ARCH_ALIGNMENT.md` as the architecture truth source, and update older design docs when they drift.
- Do not add new capabilities before policy, audit, tests, and persistence are covered for that capability.

## Completion Snapshot

| Area | Current Status | Evidence | Remaining Gap |
| --- | --- | --- | --- |
| Core Runtime | Mostly implemented MVP | `AgentLoop`, `Router`, `SkillRouter`, `AgentRouter`, `WorkflowRouter`, `PolicyEngine`, `AuditLogger`, registries | Failure recovery, lifecycle/session semantics |
| Builtin Skills | Implemented MVP | `file_read`, `file_write`, `file_patch`, `http_fetch`, `workflow_run` | Schema validation, richer workflow definitions |
| CLI Integration | Implemented MVP | `CliHost`, CLI skill invoker, repo-local external spec loader, `jq_transform`, cwd/timeout/output/env controls | Redaction, resource limits |
| Agent System | Partial | `IAgentAdapter`, `mock_planner`, `codex_cli`, `SubagentManager`, automatic subagent candidate selection, `WorkspaceSession` abstraction, subagent cost/concurrency limits | Task decomposition, role assignment, more adapters |
| Auth System | Partial | Auth manager, provider adapters, API-key env refs, CLI session probes/import tests, refresh command/adapter path, workspace default profile mapping, credential store dev-fallback status | OAuth token exchange, system credential store, full multi-account strategy |
| Memory And Evolution | Partial | Task/step logs, skill/agent stats, LessonStore, lesson-driven routing/policy hints, workflow candidates/scoring, durable WorkflowStore, promotion command, stored workflow execution, Router workflow preference, `required_inputs` applicability | Richer condition expressions |
| Identity / Trust | Implemented MVP | Identity store, pairing, allowlist, TrustPolicy | Pairing handshake UX, role/user-level authorization, device lifecycle |
| Scheduler | Implemented MVP | persisted one-shot/interval tasks, `run-due`, foreground `tick`/`daemon` loops, run history metadata, retry/backoff, small recurrence grammar, full 5-field cron + aliases, timezone (UTC / fixed offsets / curated IANA zones with DST), spring-forward/fall-back DST handling, `missed_run_policy`, backward-compatible TSV | Full IANA tzdb integration |
| Policy / Permissions | Implemented MVP | PermissionModel, risk parsing, unknown permission deny | Role-based permission grants, approval workflow |
| Plugin Host | Not implemented | Docs only | JSON-RPC/stdio plugin runtime, plugin manifest, sandboxing |
| Storage | Prototype | TSV files under `runtime/` | SQLite or versioned storage, migration, locking |
| Tests | Smoke coverage | `agentos_smoke_tests` | Unit tests per module, CI, failure/regression suites |
| Docs | Mostly current | `ARCH_ALIGNMENT.md`, `ROADMAP.md`, and this plan are synced | Some design docs still need ongoing gap annotations as implementation changes |

## Review Findings

- `docs/ROADMAP.md` has now been synced to the current implementation state, but it must stay linked to this plan to avoid drifting again.
- `AUTH_PRD.md` and `AUTH_DESIGN.md` describe OAuth, refresh, cloud credentials, and secure credential storage, but the current code only implements API-key env references, Codex/Claude CLI session probing, and refresh command plumbing without real OAuth exchange.
- Workflow learning now has candidate/scoring output, LessonStore, lesson-driven routing/policy hints, durable WorkflowStore, manual promotion, stored workflow execution, Router preference, and `required_inputs` applicability checks.
- Scheduler supports manual `run-due`, foreground `tick`, foreground `daemon`, run history metadata, retry/backoff, `missed_run_policy=run-once|skip`, `every:<n>s|m|h|d` recurrence, full five-field cron expressions with `@hourly`/`@daily`/`@weekly`/`@monthly`/`@yearly` aliases and DOM/DOW OR semantics, and timezone-aware cron evaluation (UTC, `UTC±HH:MM` fixed offsets, and a curated set of named IANA zones with built-in DST rules: US post-2007, EU post-1996, AU post-2008; fixed-offset Asian zones). DST gaps are skipped to the first valid post-gap minute and DST folds fire at the earliest occurrence only so a `30 1 * * *` cron does not double-fire when DST ends. Disabled tasks are skipped, and missed cron/interval tasks can either run once per tick or skip stale runs and reschedule from the current scheduler time. The scheduler TSV format is backward-compatible: legacy rows without `cron_expression`/`timezone_name` columns load as UTC.
- Multi-agent orchestration supports explicit lists, automatic healthy/capability-based subagent candidate selection, `WorkspaceSession` for session-capable agents, parallel concurrency limits, and estimated-cost budget checks. There is no automatic task decomposition or role assignment.
- Plugin Host is still docs-only. External CLI spec loading now supports repo-local `runtime/cli_specs/*.tsv` files.
- Persistence is TSV-based and adequate for MVP, but not yet versioned, transactional, or migration-safe.

## Next Execution Plan

### Phase A: Documentation Truth And Planning

- [x] Create `plan.md` with completion review and TODO plan.
- [x] Update `docs/ROADMAP.md` to reflect actual current implementation status.
- [x] Add an implementation status table to `README.md` or link this plan from `README.md`.
- [x] Update `docs/ARCHITECTURE.md` directory layout to include `auth`, `scheduler`, `trust`, and `core/orchestration`.
- [x] Add "Known Gaps" sections to stale design docs rather than leaving aspirational text unmarked.

### Phase B: Auth Completion

- [x] Add `auth refresh` command and adapter interface coverage.
- [ ] Implement real OAuth refresh token exchange once OAuth sessions exist.
- [x] Add workspace default profile mapping.
- [x] Implement system credential store integration or clearly mark local fallback as insecure/dev-only.
- [x] Implement OAuth PKCE skeleton for Gemini or explicitly defer OAuth from MVP.
- [x] Add tests for logout, status reload, and missing env refs.
- [x] Add CLI session import behavior tests with controllable CLI fixtures.

### Phase C: Scheduler Hardening

- [x] Add `schedule tick` command for foreground due-task execution.
- [x] Add `schedule daemon` mode or service wrapper for long-running background execution.
- [x] Add cron expression support or a documented smaller recurrence grammar.
- [x] Add retry/backoff fields to `ScheduledTask`.
- [x] Record scheduler execution metadata separately from task execution logs.
- [x] Add tests for missed tasks, disabled tasks, failed tasks, and recurring task persistence.
- [x] Add configurable missed-run policy for interval tasks.
- [x] Add timezone field on `ScheduledTask` and evaluate cron in that zone, including DST gap (skip-forward) and fall-back (fire-once) handling.

### Phase D: Workflow Evolution

- [x] Create a durable `WorkflowStore` skeleton separate from candidate generation.
- [x] Add workflow promotion command, for example `agentos memory promote-workflow <name>`.
- [x] Teach Router to prefer enabled stable workflows by trigger match and score ordering.
- [x] Extend `workflow_run` to execute stored workflow definitions, not only built-in workflows.
- [x] Add required-input workflow applicability checks beyond trigger task type.
- [x] Add `LessonStore` for repeated failure patterns.
- [x] Use lessons as routing hints.
- [x] Use lessons as policy hints.

### Phase E: Agent And Subagent System

- [x] Split Router internals into SkillRouter, AgentRouter, and WorkflowRouter without changing public behavior.
- [x] Add automatic agent candidate selection for SubagentManager.
- [x] Add WorkspaceSession abstraction for multi-agent work.
- [x] Add cost/concurrency limits for parallel subagent execution.
- [ ] Add provider adapters beyond Codex CLI only after auth/session boundaries are clear.

### Phase F: CLI And Plugin Ecosystem

- [x] Add external CLI spec loader from a repo-local spec directory.
- [x] Add `jq_transform` CLI skill or explicitly remove it from Roadmap MVP.
- [ ] Add command redaction support for sensitive arguments.
- [ ] Add process resource controls where OS support is available.
- [ ] Design and implement a minimal Plugin Host manifest and stdio protocol.

### Phase G: Safety And Policy

- [ ] Enforce `idempotency_key` for side-effecting skills where replay can duplicate effects.
- [ ] Add explicit approval policy hooks for high-risk operations.
- [ ] Add role/user-level permission grants on top of the current PermissionModel.
- [ ] Add audit coverage for trust/pairing/identity mutations.
- [ ] Add secret redaction tests for audit and CLI output paths.

### Phase H: Storage And Reliability

- [ ] Decide whether TSV remains MVP storage or migrate to SQLite.
- [ ] Add file locking or single-writer guard for runtime stores.
- [ ] Add storage versioning and migration helpers.
- [ ] Add crash-safe writes for allowlist, identities, scheduler tasks, memory stats, and execution cache.
- [ ] Add import/export tooling for runtime state.

### Phase I: Testing And CI

- [ ] Split smoke tests into module-level unit tests.
- [ ] Add CLI integration tests for each command group.
- [ ] Add negative-path tests for policy, trust, scheduler, and auth.
- [ ] Add GitHub Actions or documented local CI command.
- [ ] Add fixtures for cross-platform command execution differences.

## Immediate Next TODO

- [x] Sync `docs/ROADMAP.md` with this plan.
- [x] Link `plan.md` from `README.md`.
- [x] Add audit events for `trust identity-*`, `trust pair`, `trust block`, and `trust remove`.
- [x] Add `schedule tick` command.
- [x] Record scheduler execution metadata separately from task execution logs.
- [x] Add retry/backoff fields to `ScheduledTask`.
- [x] Add cron expression support or a documented smaller recurrence grammar.
- [x] Add tests for missed tasks and disabled tasks.
- [x] Add `WorkflowStore` skeleton.
- [x] Add workflow promotion command.
- [x] Extend `workflow_run` to execute stored workflow definitions.
- [x] Teach Router to prefer promoted workflows when applicable.
- [x] Add richer workflow applicability conditions beyond trigger task type.
- [x] Add `LessonStore` skeleton.
- [x] Use lessons as routing hints.
- [x] Use lessons as policy hints.
- [x] Add `auth refresh` command and adapter interface coverage.
- [x] Add workspace default profile mapping.
- [x] Implement system credential store integration or clearly mark local fallback as insecure/dev-only.
- [x] Implement OAuth PKCE skeleton for Gemini or explicitly defer OAuth from MVP.
- [x] Add tests for logout, status reload, and missing env refs.
- [x] Add CLI session import behavior tests with controllable CLI fixtures.

## Progress Log

- 2026-04-23: Reviewed current code/docs completion and created this plan.
- 2026-04-23: Synced `docs/ROADMAP.md` to actual implementation status and linked `plan.md` from `README.md`.
- 2026-04-23: Updated `docs/ARCHITECTURE.md` directory layout and added current implementation gaps to auth docs.
- 2026-04-23: Added trust mutation audit events and verified `trust pair` writes audit records.
- 2026-04-23: Added `schedule tick` foreground loop and verified it executes a due write task.
- 2026-04-23: Added durable `WorkflowStore` skeleton backed by `runtime/memory/workflows.tsv` and smoke-tested persistence.
- 2026-04-23: Added `memory promote-workflow` and verified candidate promotion into `workflows.tsv`.
- 2026-04-23: Extended `workflow_run` to execute promoted WorkflowStore definitions within declared workflow permissions.
- 2026-04-23: Taught Router to prefer enabled promoted workflows for matching task types and verified automatic workflow routing.
- 2026-04-23: Added `required_inputs` workflow applicability checks and verified Router skips workflows when inputs are missing.
- 2026-04-23: Added `LessonStore` backed by `runtime/memory/lessons.tsv` and verified repeated failure aggregation.
- 2026-04-23: Used LessonStore as Router hints to suppress repeated workflow failures and penalize repeatedly failing agents.
- 2026-04-23: Added LessonStore policy hints for repeated PolicyDenied results without changing hard policy decisions.
- 2026-04-23: Added `auth refresh` command, AuthManager refresh flow, adapter coverage, and unsupported-refresh handling.
- 2026-04-23: Added workspace auth default profile mapping backed by `runtime/auth_profiles.tsv`.
- 2026-04-23: Added `auth credential-store` and SecureTokenStore status to explicitly mark the env-ref-only dev fallback.
- 2026-04-23: Explicitly deferred Browser OAuth / PKCE from MVP and covered Gemini OAuth with `BrowserOAuthNotImplemented`.
- 2026-04-23: Added Auth tests for logout, SessionStore reload status, and missing environment credential refs.
- 2026-04-23: Added scheduler run history metadata backed by `runtime/scheduler/runs.tsv` and `schedule history`.
- 2026-04-23: Added scheduler retry/backoff fields and verified failed tasks retry before disabling.
- 2026-04-23: Added small scheduler recurrence grammar `every:<n>s|m|h|d` as an MVP alternative to full cron.
- 2026-04-23: Added scheduler regression coverage for disabled tasks and missed interval tasks.
- 2026-04-23: Added controllable Codex/Claude CLI session fixture tests for auth probe/import success and unavailable-session failure.
- 2026-04-23: Added `schedule daemon` foreground loop as the long-running scheduler wrapper, reusing the `tick` execution path.
- 2026-04-23: Added `missed_run_policy=run-once|skip` for interval tasks and covered skip semantics.
- 2026-04-23: Split Router internals into SkillRouter, AgentRouter, and WorkflowRouter while preserving public selection behavior.
- 2026-04-23: Added automatic SubagentManager candidate selection by healthy capability match, historical stats, and lessons.
- 2026-04-23: Added WorkspaceSession abstraction for opening, using, and closing session-capable agent adapters within a workspace.
- 2026-04-23: Added SubagentManager parallel concurrency limits and estimated-cost budget checks with memory cost stats.
- 2026-04-23: Added repo-local external CLI spec loading from `runtime/cli_specs/*.tsv` and verified dynamic skill registration.
- 2026-04-23: Added built-in `jq_transform` CLI skill backed by jq and covered it with a controllable CLI fixture.

## Post-Review Updates

- 2026-04-26: Added five-field cron grammar (with `@hourly`/`@daily`/`@weekly`/`@monthly`/`@yearly` aliases, DOM/DOW OR semantics) and a lightweight Timezone module (UTC, `UTC±HH:MM` fixed offsets, curated IANA zones with built-in US/EU/AU DST rules). `ScheduledTask` gains `cron_expression` and `timezone_name` fields persisted as two new TSV columns. Scheduler reschedules cron tasks via timezone-aware next-fire computation; spring-forward gaps skip to the first valid post-gap minute and fall-back folds fire only at the earliest occurrence. CLI accepts `cron=` and `timezone=` and rejects invalid values with `InvalidCronExpression` / `TimezoneUnknown`. Legacy TSV rows without the new columns still load (defaulting to UTC).
