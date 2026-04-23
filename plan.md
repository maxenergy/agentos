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
| Core Runtime | Mostly implemented MVP | `AgentLoop`, `Router`, `PolicyEngine`, `AuditLogger`, registries | Failure recovery, lifecycle/session semantics, router decomposition |
| Builtin Skills | Implemented MVP | `file_read`, `file_write`, `file_patch`, `http_fetch`, `workflow_run` | Schema validation, richer workflow definitions |
| CLI Integration | Implemented MVP | `CliHost`, CLI skill invoker, cwd/timeout/output/env controls | External spec loader, `jq_transform`, resource limits |
| Agent System | Partial | `IAgentAdapter`, `mock_planner`, `codex_cli`, `SubagentManager` | Auto multi-agent routing, WorkspaceSession, more adapters |
| Auth System | Partial | Auth manager, provider adapters, API-key env refs, CLI session probes | OAuth, refresh, system credential store, workspace default profiles |
| Memory And Evolution | Partial | Task/step logs, skill/agent stats, LessonStore, workflow candidates/scoring, durable WorkflowStore, promotion command, stored workflow execution, Router workflow preference, `required_inputs` applicability | Lesson-driven policy/routing hints, richer condition expressions |
| Identity / Trust | Implemented MVP | Identity store, pairing, allowlist, TrustPolicy | Pairing handshake UX, role/user-level authorization, device lifecycle |
| Scheduler | Partial MVP | persisted one-shot/interval tasks, `run-due`, foreground `tick` loop | Daemon/service wrapper, cron, retry/backoff, missed-run policy |
| Policy / Permissions | Implemented MVP | PermissionModel, risk parsing, unknown permission deny | Role-based permission grants, approval workflow |
| Plugin Host | Not implemented | Docs only | JSON-RPC/stdio plugin runtime, plugin manifest, sandboxing |
| Storage | Prototype | TSV files under `runtime/` | SQLite or versioned storage, migration, locking |
| Tests | Smoke coverage | `agentos_smoke_tests` | Unit tests per module, CI, failure/regression suites |
| Docs | Mostly current | `ARCH_ALIGNMENT.md`, `ROADMAP.md`, and this plan are synced | Some design docs still need ongoing gap annotations as implementation changes |

## Review Findings

- `docs/ROADMAP.md` has now been synced to the current implementation state, but it must stay linked to this plan to avoid drifting again.
- `AUTH_PRD.md` and `AUTH_DESIGN.md` describe OAuth, refresh, cloud credentials, and secure credential storage, but the current code only implements API-key env references plus Codex/Claude CLI session probing.
- Workflow learning now has candidate/scoring output, LessonStore, durable WorkflowStore, manual promotion, stored workflow execution, Router preference, and `required_inputs` applicability checks.
- Scheduler supports manual `run-due` and foreground `tick`; there is no daemon/service wrapper, cron parser, retry policy, or missed-run semantics.
- Multi-agent orchestration is explicit only. There is no automatic task decomposition, role assignment, WorkspaceSession, or cost-aware multi-agent router.
- Plugin Host and external CLI spec loading are still docs-only.
- Persistence is TSV-based and adequate for MVP, but not yet versioned, transactional, or migration-safe.

## Next Execution Plan

### Phase A: Documentation Truth And Planning

- [x] Create `plan.md` with completion review and TODO plan.
- [x] Update `docs/ROADMAP.md` to reflect actual current implementation status.
- [x] Add an implementation status table to `README.md` or link this plan from `README.md`.
- [x] Update `docs/ARCHITECTURE.md` directory layout to include `auth`, `scheduler`, `trust`, and `core/orchestration`.
- [x] Add "Known Gaps" sections to stale design docs rather than leaving aspirational text unmarked.

### Phase B: Auth Completion

- [ ] Add `auth refresh` command and adapter interface coverage.
- [ ] Add workspace default profile mapping.
- [ ] Implement system credential store integration or clearly mark local fallback as insecure/dev-only.
- [ ] Implement OAuth PKCE skeleton for Gemini or explicitly defer OAuth from MVP.
- [ ] Add tests for logout, status reload, missing env refs, and CLI session import behavior.

### Phase C: Scheduler Hardening

- [x] Add `schedule tick` command for foreground due-task execution.
- [ ] Add `schedule daemon` mode or service wrapper for long-running background execution.
- [ ] Add cron expression support or a documented smaller recurrence grammar.
- [ ] Add retry/backoff fields to `ScheduledTask`.
- [ ] Record scheduler execution metadata separately from task execution logs.
- [ ] Add tests for missed tasks, disabled tasks, failed tasks, and recurring task persistence.

### Phase D: Workflow Evolution

- [x] Create a durable `WorkflowStore` skeleton separate from candidate generation.
- [x] Add workflow promotion command, for example `agentos memory promote-workflow <name>`.
- [x] Teach Router to prefer enabled stable workflows by trigger match and score ordering.
- [x] Extend `workflow_run` to execute stored workflow definitions, not only built-in workflows.
- [x] Add required-input workflow applicability checks beyond trigger task type.
- [x] Add `LessonStore` for repeated failure patterns.
- [ ] Use lessons as policy/routing hints.

### Phase E: Agent And Subagent System

- [ ] Split Router internals into SkillRouter, AgentRouter, and WorkflowRouter without changing public behavior.
- [ ] Add automatic agent candidate selection for SubagentManager.
- [ ] Add WorkspaceSession abstraction for multi-agent work.
- [ ] Add cost/concurrency limits for parallel subagent execution.
- [ ] Add provider adapters beyond Codex CLI only after auth/session boundaries are clear.

### Phase F: CLI And Plugin Ecosystem

- [ ] Add external CLI spec loader from a repo-local spec directory.
- [ ] Add `jq_transform` CLI skill or explicitly remove it from Roadmap MVP.
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
- [x] Add `WorkflowStore` skeleton.
- [x] Add workflow promotion command.
- [x] Extend `workflow_run` to execute stored workflow definitions.
- [x] Teach Router to prefer promoted workflows when applicable.
- [x] Add richer workflow applicability conditions beyond trigger task type.
- [x] Add `LessonStore` skeleton.
- [ ] Use lessons as policy/routing hints.

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
