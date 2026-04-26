# AgentOS Completion Review And Plan

Last updated: 2026-04-26

This file is the working plan for aligning the implementation with the docs. Update it whenever a task is completed, deferred, or re-scoped.

## Update Rules

- Mark completed tasks with `[x]` in the same commit/change set that implements them.
- Add a short note under "Progress Log" for every completed milestone.
- Keep `docs/ARCH_ALIGNMENT.md` as the architecture truth source, and update older design docs when they drift.
- Do not add new capabilities before policy, audit, tests, and persistence are covered for that capability.

## Completion Snapshot

Current review: AgentOS is a runnable local MVP, not a production-complete system. Rough completion is about 80% for MVP scope and 55-60% for product-grade scope. The latest local verification passes all 11 CTest targets.

| Area | Current Status | Evidence | Remaining Gap |
| --- | --- | --- | --- |
| Core Runtime | Mostly implemented MVP | `AgentLoop`, `Router`, `SkillRouter`, `AgentRouter`, `WorkflowRouter`, `PolicyEngine`, `AuditLogger`, registries | Failure recovery, lifecycle/session semantics |
| Builtin Skills | Implemented MVP | `file_read`, `file_write`, `file_patch`, `http_fetch`, `workflow_run`, required input schema validation, scalar `properties.*.type`, `const`, `enum`, length, pattern, `additionalProperties:false`, `propertyNames`, `minProperties` / `maxProperties`, inclusive/exclusive numeric range, `multipleOf`, `dependentRequired`, legacy `dependencies`, object-level `not.required`, basic `if` / `then` / `else`, and `allOf` / `anyOf` / `oneOf` required-branch validation | Richer JSON Schema keywords and richer workflow definitions |
| CLI Integration | Implemented MVP | `CliHost`, CLI skill invoker, repo-local external spec loader with source file/line metadata, empty-field-safe TSV parsing, duplicate-name diagnostics, registered-skill conflict audit/validate checks, parse-mode/risk/permission diagnostics, required-arg/template consistency diagnostics, schema-object diagnostics, strict numeric-field diagnostics with positive timeout and non-negative resource limits, startup audit events for skipped specs, `agentos cli-specs validate`, `jq_transform`, cwd/timeout/output/env controls, sensitive argument redaction, Windows Job Object and POSIX `setrlimit` resource controls for process/memory/CPU limits, plus POSIX file descriptor limits | Windows file-handle limits and richer sandboxing |
| Agent System | Partial | `IAgentAdapter`, `local_planner` local deterministic planning adapter with session tracking and structured plan output, `codex_cli`, `gemini` generateContent provider adapter, `anthropic` Messages/Claude CLI adapter, `qwen` OpenAI-compatible Chat Completions adapter, normalized `agent_result.v1` structured output, `SubagentManager`, deterministic role assignment, explicit and planner-generated per-agent subtask objective inputs, step-level structured output retention and normalized `agent_outputs` aggregation, automatic subagent candidate selection, `WorkspaceSession` abstraction, subagent cost/concurrency limits | More advanced model-driven complex task decomposition |
| Auth System | Partial | Auth manager, provider adapters, API-key env refs, Gemini CLI browser-OAuth passthrough import, CLI session probes/import tests, refresh command/adapter path, workspace default profile mapping, `auth profiles` session/profile discovery, `set_default=true` one-step default profile selection for login/OAuth completion, Windows Credential Manager backend for managed tokens, non-Windows credential store dev-fallback status, reusable OAuth PKCE start/callback URL validation scaffolding, Gemini Google OAuth default authorization/token endpoints and scopes, repo-local `runtime/auth_oauth_providers.tsv` OAuth defaults overrides, `auth oauth-config-validate` diagnostics, `auth oauth-defaults` provider OAuth discovery, `auth oauth-start` authorization URL generation with optional `open_browser=true` system-browser launch, `auth oauth-login` start/listen/token-exchange/session-persist orchestration, `auth oauth-callback` callback URL parsing/state validation, `auth oauth-listen` one-shot loopback callback capture, authorization-code and refresh-token request form-body construction, curl-backed exchange helpers, token response parsing, managed AuthSession persistence helpers, `auth oauth-complete`, parameterized provider adapter native OAuth login/refresh completion, `auth oauth-token-request`, and `auth oauth-refresh-request` | Additional provider-specific OAuth discovery, fuller multi-provider interactive login UX, non-Windows system credential stores, full multi-account strategy |
| Memory And Evolution | Partial | Task/step logs, skill/agent stats, LessonStore, lesson-driven routing/policy hints, workflow candidates/scoring, durable WorkflowStore, promotion command with validation, stored workflow execution, Router workflow preference, required-input, exact-match, numeric-range, boolean-input, regex-input, `input_any` composite OR, `input_expr` nested boolean applicability, filtered `memory stored-workflows`, `memory show-workflow`, `memory clone-workflow`, `memory update-workflow`, `memory set-workflow-enabled`, `memory remove-workflow`, `memory validate-workflows`, and `memory explain-workflow` | Richer workflow definition editing UX |
| Identity / Trust | Implemented MVP | Identity store, one-time pairing invite handshake with TTL, paired-device lifecycle metadata and show/manage commands, pairing, allowlist, TrustPolicy, per-task permission grants, persistent RoleCatalog user-role grants | Richer device fleet admin UX |
| Scheduler | Implemented MVP | persisted one-shot/interval/cron tasks, `run-due`, foreground `tick` loop, foreground `daemon` loop, run history metadata, retry/backoff, `every:<n>s|m|h|d` recurrence, five-field cron expressions plus `@hourly` / `@daily` / `@weekly` / `@monthly` / `@yearly` / `@annually` aliases, DOM/DOW OR semantics, `missed_run_policy`, disabled/missed-run coverage | Timezone/DST-specific calendar hardening if required |
| Policy / Permissions | Implemented MVP | PermissionModel, risk parsing, unknown permission deny, per-task permission grants, persistent RoleCatalog user-role grants plus remove/show commands, persistent ApprovalStore with request/show/approve/revoke and approved approval_id enforcement for CLI runtime, side-effect idempotency enforcement, CLI/audit sensitive value redaction | Richer admin UX |
| Plugin Host | Partial | `PluginHost`, `PluginSkillInvoker`, `runtime/plugin_specs/*.tsv`, `runtime/plugin_specs/*.json`, `plugin.v1` TSV/JSON manifest, source file/line metadata for loaded/skipped/conflicting specs, empty-field-safe TSV parsing, duplicate-name diagnostics, registered-skill conflict audit/validate checks, risk/permission diagnostics including required `process.spawn`, required-arg/template consistency diagnostics, health-probe placeholder diagnostics, schema-object diagnostics, strict numeric/bool-field diagnostics with positive timeout and non-negative resource limits, spec health validation/reporting, optional `health_args_template` probes, skipped-spec diagnostics with startup audit events, `agentos plugins`, scriptable `agentos plugins validate`, scriptable `agentos plugins health`, scriptable `agentos plugins lifecycle`, scriptable `agentos plugins inspect name=<plugin> [health=true]` manifest inspection and optional single-plugin health probe, `stdio-json-v0` JSON-object stdout and `json-rpc-v0` JSON-RPC 2.0 result-object process protocols, persistent `json-rpc-v0` process sessions with request IDs, stdin/stdout round-trips, timeout handling, stderr capture, failure-session eviction/restart, destructor shutdown, lifecycle-aware health round-trips, active-session counting, manual close, idle-timeout restart, workspace-configurable `runtime/plugin_host.tsv` max persistent session cap with LRU eviction, `output_schema_json.required` / `properties.*.type` / string `const` / `enum` / length / pattern / `additionalProperties:false` / `propertyNames` / `minProperties` / `maxProperties` / `dependentRequired` / `dependencies` / `not.required` / `if` / `then` / `else` / numeric range / `multipleOf` / `allOf` / `anyOf` / `oneOf` required-branch validation, structured `plugin_output`, CLI resource limit pass-through, and `sandbox_mode=workspace|none` with workspace path-argument containment | Deeper process-pool policy and richer runtime session admin UX |
| Storage | Prototype | TSV files under `runtime/`, atomic replacement for full-file TSV stores, locked append-only logs with append-intent recovery and same-process concurrent writer coverage, transactional multi-file write helper with prepare/commit/recover markers, `storage recover` with corrupt committed transaction isolation and `failed=` reporting, `runtime/storage_manifest.tsv` version metadata, `storage status` file existence/bytes/line-count inspection, `storage verify [src=<directory>] [strict=true]` manifest completeness diagnostics, manifest-based export/import with pre-import backups under `runtime/.import_backups/`, `storage backups` discoverability and `storage restore-backup` rollback for import backup directories, explicit `storage migrate` helper with legacy path migration into `runtime/` plus schema normalization for memory/workflow/scheduler stores, storage compaction for task/step/scheduler logs, audit compaction plus task lifecycle rebuild from `runtime/memory` and scheduler-run rebuild from `runtime/scheduler/runs.tsv`, explicit TSV MVP policy | SQLite or broader cross-format migration, fuller cross-process concurrency testing and audit recovery |
| Tests | Module-level coverage + CLI integration + basic CI | `agentos_file_skill_policy_tests`, `agentos_cli_integration_tests`, `agentos_storage_tests`, `agentos_auth_tests`, `agentos_agent_provider_tests`, `agentos_policy_trust_tests`, `agentos_scheduler_tests`, `agentos_workflow_router_tests`, `agentos_subagent_session_tests`, `agentos_cli_plugin_tests`, `agentos_spec_parsing_tests`; GitHub Actions configure/build/test on Windows + Ubuntu; added negative-path coverage across policy/trust/scheduler/auth plus shared cross-platform command fixtures | Expand failure/regression suites |
| Docs | Mostly current | `ARCH_ALIGNMENT.md`, `ROADMAP.md`, and this plan are synced | Some design docs still need ongoing gap annotations as implementation changes |

## Review Findings

- Current workspace state passes `cmake --build build && ctest --test-dir build --output-on-failure` with 11/11 tests passing.
- The system is broad enough for local MVP validation, but production readiness still depends on auth hardening, plugin long-running lifecycle, stronger storage recovery, and richer agent orchestration.
- `docs/ROADMAP.md` has now been synced to the current implementation state, but it must stay linked to this plan to avoid drifting again.
- `AUTH_PRD.md` and `AUTH_DESIGN.md` describe OAuth, refresh, cloud credentials, and secure credential storage; current code implements API-key env references, Windows Credential Manager managed-token storage, Codex/Claude CLI session probing, Gemini CLI browser-OAuth passthrough, native PKCE authorization-code / refresh-token exchange helpers, managed session persistence, Gemini OAuth defaults, repo-local OAuth defaults overrides, `oauth-config-validate`, `auth profiles`, `set_default=true`, and `oauth-login` single-command OAuth orchestration. Remaining Auth gaps are broader provider-specific OAuth discovery, fuller multi-provider product UX, non-Windows credential stores, cloud credential modes, and a complete multi-account strategy.
- Workflow learning now has candidate/scoring output, LessonStore, lesson-driven routing/policy hints, durable WorkflowStore, manual promotion with validation, stored workflow execution, Router preference, required-input / exact-match / numeric-range / boolean-input / regex-input / `input_any` composite OR / `input_expr` nested boolean applicability checks, filtered `memory stored-workflows`, `memory show-workflow` inspection, `memory clone-workflow`, `memory update-workflow`, `memory set-workflow-enabled`, `memory remove-workflow`, `memory validate-workflows` diagnostics, and `memory explain-workflow` applicability explanations.
- Scheduler supports manual `run-due`, foreground `tick`, foreground `daemon`, run history metadata, retry/backoff, `missed_run_policy=run-once|skip`, `every:<n>s|m|h|d` recurrence, and five-field cron expressions with `*`, `*/n`, single values, comma lists, ranges, `@hourly` / `@daily` / `@weekly` / `@monthly` / `@yearly` / `@annually` aliases, and DOM/DOW OR semantics. Disabled tasks are skipped, and missed interval/cron tasks can either run once per tick or skip stale runs and reschedule from the current scheduler time.
- Multi-agent orchestration supports explicit lists, deterministic role assignment, explicit per-agent subtask objectives, `auto_decompose=true` planner-generated subtask objectives, step-level structured output retention, top-level normalized `agent_outputs` aggregation, automatic healthy/capability-based subagent candidate selection, `WorkspaceSession` for session-capable agents, parallel concurrency limits, and estimated-cost budget checks. More advanced model-driven complex task decomposition remains open.
- Plugin Host now has repo-local `plugin.v1` manifests with `stdio-json-v0` JSON-object stdout and `json-rpc-v0` JSON-RPC 2.0 result-object process protocols through `runtime/plugin_specs/*.tsv` and `runtime/plugin_specs/*.json`, source file/line metadata for loaded/skipped/conflicting specs, empty-field-safe TSV parsing, duplicate-name diagnostics, registered-skill conflict audit/validate checks, risk/permission diagnostics including required `process.spawn`, required-arg/template consistency diagnostics, health-probe placeholder diagnostics, schema-object diagnostics, strict numeric/bool-field diagnostics, positive timeout and non-negative resource limit checks, structured health reporting with optional `health_args_template` probes, skipped-spec diagnostics, startup audit events for skipped specs, `agentos plugins`, scriptable `agentos plugins validate`, scriptable `agentos plugins health`, scriptable `agentos plugins lifecycle`, scriptable `agentos plugins inspect name=<plugin> [health=true]`, `output_schema_json.required` / `properties.*.type` / string `const` / `enum` / length / pattern / `additionalProperties:false` / `propertyNames` / `minProperties` / `maxProperties` / `dependentRequired` / `dependencies` / `not.required` / `if` / `then` / `else` / numeric range / `multipleOf` / `allOf` / `anyOf` / `oneOf` required-branch validation, structured `plugin_output` embedding for successful plugin output, pass-through for CLI resource limit fields, `sandbox_mode=workspace|none` with workspace path-argument containment, reusable long-running JSON-RPC process sessions, and configurable max persistent session cap with LRU eviction; deeper process-pool policy and richer runtime session admin UX remain open. External CLI spec loading supports repo-local `runtime/cli_specs/*.tsv` files with source file/line metadata, empty-field-safe TSV parsing, duplicate-name diagnostics, registered-skill conflict audit/validate checks, parse-mode/risk/permission diagnostics, required-arg/template consistency diagnostics, schema-object diagnostics, strict numeric-field diagnostics, positive timeout and non-negative resource limit checks, startup audit events for skipped specs, and `agentos cli-specs validate` diagnostics.
- Skill execution now validates required input fields, basic scalar property types, const values, enum membership, string length, regex pattern, unexpected inputs when `additionalProperties:false`, input field-name constraints via `propertyNames`, object property count via `minProperties` / `maxProperties`, inclusive/exclusive numeric range, `multipleOf`, `dependentRequired`, legacy `dependencies`, object-level `not.required`, basic `if` / `then` / `else`, and `allOf` / `anyOf` / `oneOf` required-branch constraints declared in `input_schema_json` before invoking the selected skill.
- Full-file TSV stores now write through a temporary file and atomic replacement to reduce crash-corruption risk.
- Append-only logs now use the same single-writer lock helper before appending audit/task/step/scheduler history records, and write append-intent files so the next append can recover an interrupted write or remove a stale already-applied intent.
- Runtime now maintains `runtime/storage_manifest.tsv` so storage format/version metadata is explicit and inspectable.
- `agentos storage migrate` now makes manifest/version normalization an explicit maintenance action instead of only an implicit startup side effect.
- `agentos storage migrate` now also migrates legacy root-level storage files into the current `runtime/` layout, normalizes known legacy memory/workflow/scheduler schemas to the current TSV shape, and reports `migrated_files`.
- `agentos storage export dest=...` can export manifest-managed runtime state to a separate directory.
- `agentos storage import src=...` can restore manifest-managed runtime state from an exported directory and first backs up existing managed files under `runtime/.import_backups/`.
- `agentos storage backups` lists available pre-import backup directories with relative paths, file counts, byte counts, and a summary for scripting rollback workflows.
- `agentos storage restore-backup name=<backup_name>` restores a listed pre-import backup by directory name and backs up the current overwritten files before rollback.
- `agentos storage compact` now rewrites task/step/scheduler logs from parsed state and can rebuild task-related audit lifecycle entries from `runtime/memory` plus `scheduler_run` audit events from `runtime/scheduler/runs.tsv` even if `audit.log` is missing, while preserving non-task audit events, reinserting task-scoped preserved events like `policy` alongside the recovered task flow, replacing stale scheduler-run audit rows with scheduler history state, preserving orphan task-scoped preserved events for tasks no longer present in memory logs, reusing prior lifecycle timestamps when available, merging task bundles, scheduler-run chunks, and timestamped preserved task events into a stable chronological order, and placing untimed global preserved events after timed chunks in original order.
- `agentos storage status` now reports the current storage policy and per-component file existence, byte size, and line count; `agentos storage verify [src=<directory>] [strict=true]` provides scriptable manifest completeness diagnostics before export/import workflows; TSV remains the MVP backend and SQLite migration is deferred until stronger transactional/query needs appear.
- Persistence is TSV-based and adequate for MVP, but not yet transactional and still lacks broader cross-format migration and full-fidelity audit reconstruction.
- Identity / Trust now includes a persisted pairing-invite handshake: `trust invite-create` creates one-time TTL-bound invites, `trust invite-accept` consumes them into the allowlist, and `trust invites` lists active invitations.
- Paired devices now carry lifecycle metadata (`paired_epoch_ms` and `last_seen_epoch_ms`) and can be renamed, marked seen, blocked, unblocked, or removed through `agentos trust`.
- Policy approvals now have persistent records: `trust approval-request` creates a pending approval, `trust approval-approve` authorizes it, `trust approval-revoke` revokes it, and CLI runtime high-risk policy requires `approval_id` to reference an approved record.
- Trust/Policy admin UX now includes `trust role-show`, `trust user-role-show`, and `trust approval-show` for scriptable single-record inspection with explicit not-found exits.
- Trust device admin UX now includes `trust device-show` for scriptable paired-device inspection with lifecycle metadata and explicit not-found exits.

## Next Execution Plan

### Current Highest-Priority Gaps

- Auth hardening: additional provider-specific OAuth discovery, fuller multi-provider product login UX, non-Windows system credential store integration, cloud credential modes, and a coherent multi-account strategy.
- Plugin lifecycle: persistent JSON-RPC process sessions are now implemented with a configurable session cap, LRU eviction, `plugin_host.tsv` diagnostics, and `plugins lifecycle` manifest-level admin view; remaining work is runtime session admin UX and deeper pool policy.
- Storage reliability: transactional multi-file helper is now available; remaining work is deciding when to move beyond TSV MVP and extending audit/state recovery fidelity.
- Agent orchestration: normalized cross-agent result schema is available; remaining work is richer model-driven complex task decomposition.
- Scheduler completeness: timezone/DST-specific calendar hardening if product requirements need it.

## Completion Review Validation: 2026-04-25

`completion_review.md` is mostly directionally correct for MVP/product readiness, but it is not exact against the current workspace:

- Correct with updates: `oauth-defaults` exposes built-in provider OAuth discovery, `oauth-start` can optionally launch the system browser, and `oauth-login` now orchestrates start/listen/token-exchange/session persistence in one command; broader multi-provider login UX and non-Windows `SecureTokenStore` are still missing. Plugin Host has reusable long-running JSON-RPC lifecycle support and has been split into focused modules; TSV remains the MVP storage backend. Gemini now has built-in Google OAuth defaults, while broader provider-specific OAuth configuration remains open.
- Outdated: Plugin sandboxing is no longer missing. `PluginSpec.sandbox_mode` now supports `workspace|none`, and default workspace mode rejects path-like runtime arguments that resolve outside the active workspace.
- Overstated: Phase F should not be considered fully production-complete while long-running plugin lifecycle remains open. Phase H should not be considered fully production-complete while transaction semantics and broader cross-format migration remain open.
- Current verification: full local `cmake --build build && ctest --test-dir build --output-on-failure` passed 11/11 tests after the sandbox update.

## Post-Review Development Plan

### Phase J: Auth Production Hardening

- [x] Add a credential-store abstraction that can report and select `env-ref-only` or Windows Credential Manager backends.
- [x] Implement a Windows Credential Manager backend first, with read/write/delete/status tests guarded for Windows.
- [x] Add macOS Keychain backend via the Security framework (generic password items) selected automatically on `__APPLE__`.
- [x] Add Linux Secret Service backend via libsecret (D-Bus) as an optional CMake dependency, falling back to env-ref-only when libsecret is not installed.
- [x] Refactor `SecureTokenStore` around an `ISecureTokenBackend` abstraction with a test-only in-memory backend so unit tests never reach the real keychain.
- [x] Add native OAuth PKCE scaffolding behind provider capability flags, including state/verifier generation, authorization URL output, callback URL parsing/validation, one-shot loopback callback capture, authorization-code and refresh-token request construction, curl-backed exchange helpers, token response parsing, and managed AuthSession persistence helpers.
- [x] Add a scriptable callback-to-session OAuth completion bridge through `auth oauth-complete`.
- [x] Implement parameterized provider adapter native login/refresh orchestration for callers that provide callback/token endpoint/client configuration.
- [x] Add Gemini provider-specific OAuth defaults for Google authorization/token endpoints and default scopes.
- [x] Add a single-command OAuth login bridge that orchestrates start/listen/token-exchange/session persistence for providers with OAuth defaults or explicit endpoints.
- [x] Broaden OAuth provider discovery: stub entries for `openai` / `anthropic` / `qwen` with `origin` and `note` metadata, surfaced via `auth oauth-defaults` and `auth oauth-config-validate --all` so users can see at a glance which providers ship builtin defaults vs. require workspace overrides.
- [ ] Document and ship public PKCE endpoints for OpenAI / Anthropic once the providers expose stable customer flows (currently registered as `stub` defaults).
- [ ] Polish multi-provider interactive login UX (single-binary CLI prompt with per-provider hints sourced from the new origin/note metadata).
- [x] Update `AUTH_PRD.md`, `AUTH_DESIGN.md`, and README examples after native credential storage and OAuth flows exist.

### Phase K: Plugin Long-Running Lifecycle

- [x] Add manifest lifecycle fields such as `lifecycle_mode=oneshot|persistent`, `startup_timeout_ms`, and `idle_timeout_ms`.
- [x] Split plugin manifest parsing/validation from runtime execution to reduce `plugin_host.cpp` coupling before adding process pools.
- [x] Extract shared PluginSpec support validation and PluginSpec-to-CliSpec conversion into `plugin_spec_utils.*`.
- [x] Extract `PluginSkillInvoker` implementation into `plugin_skill_invoker.cpp`.
- [x] Extract plugin workspace sandbox path containment into `plugin_sandbox.*`.
- [x] Implement a persistent JSON-RPC plugin session manager with request IDs, timeout handling, stderr capture, restart-on-failure, and explicit shutdown.
- [x] Extend `agentos plugins health` with lifecycle-aware startup/round-trip checks.
- [x] Add regression coverage for persistent success, timeout, crash/restart, malformed response, lifecycle event output, and session reuse.

### Phase L: Storage Reliability

- [x] Add a transactional batch-write helper for multi-file runtime updates with prepare/commit/recover markers.
- [x] Extend storage recovery tests to simulate interrupted multi-file writes.
- [x] Define the SQLite migration boundary as an interface decision record before adding a second backend.
- [x] Improve audit reconstruction coverage for non-task retained events and mixed scheduler/task timelines.

### Phase M: Agent Result Normalization

- [x] Define a common normalized agent result schema for `summary`, `artifacts`, `metrics`, `tool_calls`, `model`, and provider-specific metadata.
- [x] Normalize Codex/Gemini/Anthropic/Qwen/local_planner adapter outputs into that schema while preserving raw provider output.
- [x] Teach SubagentManager aggregation to consume normalized fields instead of provider-specific passthrough strings.
- [x] Add tests for mixed-provider aggregation and missing-field fallback.

### Phase N: CLI And Docs Maintenance

- [x] Keep `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, and `completion_review.md` synchronized after each completed phase.
- [x] Split `main.cpp` command groups once feature churn slows, starting with auth/plugin/storage commands.
- [x] Extract the storage command group from `main.cpp` into `src/cli/storage_commands.*`.
- [x] Extract the schedule command group from `main.cpp` into `src/cli/schedule_commands.*`.
- [x] Extract the subagents command group from `main.cpp` into `src/cli/subagents_commands.*`.
- [x] Extract the agents command from `main.cpp` into `src/cli/agents_commands.*`.
- [x] Extract the cli-specs command group from `main.cpp` into `src/cli/cli_specs_commands.*`.
- [x] Extract the plugins command group from `main.cpp` into `src/cli/plugins_commands.*`.
- [x] Extract the trust command group from `main.cpp` into `src/cli/trust_commands.*`.
- [x] Extract the memory command group from `main.cpp` into `src/cli/memory_commands.*`.
- [x] Extract the auth command group from `main.cpp` into `src/cli/auth_commands.*`.
- [x] Finish splitting `plugin_host.cpp` health and execution/session lifecycle modules after lifecycle interfaces stabilize.
- [x] Collapse `PolicyEngine` constructor overloads into an explicit dependency/options object or a single constructor shape.
- [x] Split `router_components.cpp` into distinct SkillRouter, AgentRouter, and WorkflowRouter implementation files with focused unit coverage.
- [x] Extract shared step execution/lifecycle helpers so AgentLoop and SubagentManager share policy/audit/memory step recording semantics.
- [x] Move lesson-derived policy hints out of `AgentLoop` into a single LessonHintProvider or MemoryManager helper.
- [ ] Evaluate replacing ad hoc JSON parsing paths with a structured JSON dependency before adding more schema keywords or provider protocols.

### Phase A: Documentation Truth And Planning

- [x] Create `plan.md` with completion review and TODO plan.
- [x] Update `docs/ROADMAP.md` to reflect actual current implementation status.
- [x] Add an implementation status table to `README.md` or link this plan from `README.md`.
- [x] Update `docs/ARCHITECTURE.md` directory layout to include `auth`, `scheduler`, `trust`, and `core/orchestration`.
- [x] Add "Known Gaps" sections to stale design docs rather than leaving aspirational text unmarked.

### Phase B: Auth Completion

- [x] Add `auth refresh` command and adapter interface coverage.
- [x] Implement real OAuth refresh token exchange once OAuth sessions exist.
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
- [x] Add deterministic role assignment and role-scoped subtask objectives.
- [x] Add provider adapters beyond Codex CLI only after auth/session boundaries are clear.

### Phase F: CLI And Plugin Ecosystem

- [x] Add external CLI spec loader from a repo-local spec directory.
- [x] Add `jq_transform` CLI skill or explicitly remove it from Roadmap MVP.
- [x] Add command redaction support for sensitive arguments.
- [x] Add process resource controls where OS support is available.
- [x] Design and implement a minimal Plugin Host manifest and stdio protocol.

### Phase G: Safety And Policy

- [x] Enforce `idempotency_key` for side-effecting skills where replay can duplicate effects.
- [x] Add explicit approval policy hooks for high-risk operations.
- [x] Add role/user-level permission grants on top of the current PermissionModel.
- [x] Add audit coverage for trust/pairing/identity mutations.
- [x] Add secret redaction tests for audit paths.

### Phase H: Storage And Reliability

- [x] Decide whether TSV remains MVP storage or migrate to SQLite.
- [x] Add file locking or single-writer guard for runtime stores.
- [x] Add storage versioning metadata and basic helper plumbing.
- [x] Add explicit storage migration helper for manifest/version normalization.
- [x] Add explicit legacy path migration into the `runtime/` storage layout.
- [x] Add crash-safe writes for allowlist, identities, scheduler tasks, memory stats, and execution cache.
- [x] Add export tooling for runtime state.
- [x] Add import tooling for runtime state.

### Phase I: Testing And CI

- [x] Split smoke tests into module-level unit tests.
- [x] Add CLI integration tests for each command group.
- [x] Add negative-path tests for policy, trust, scheduler, and auth.
- [x] Add GitHub Actions or documented local CI command.
- [x] Add fixtures for cross-platform command execution differences.

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

## Post-Review Work

Tracking items raised by `completion_review.md` that fall outside the original
Phase A–I plan. These are research / decision items rather than direct
implementation tasks.

- [ ] **Structured JSON dependency** — `completion_review.md` §6 ⚠️ flags that
  the repo still uses hand-rolled `json_utils` and bespoke parse/validate
  paths for schema, provider, and plugin protocols. Decision recorded in
  [`docs/ADR-JSON-001.md`](docs/ADR-JSON-001.md) (Status: Proposed,
  recommends nlohmann/json with a phased migration plan; no code changes
  applied yet). Resolves only after the migration phases in that ADR are
  executed.
- [ ] **Storage backend boundary** — already tracked under Phase H; the
  long-form decision is captured in `ADR-STORAGE-001` (TSV remains MVP,
  SQLite deferred).

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
- 2026-04-25: Added Windows Credential Manager managed-token backend to SecureTokenStore, with read/write/delete/status coverage while preserving env-ref support.
- 2026-04-25: Added reusable OAuth PKCE start/callback scaffolding with S256 challenge generation, `auth oauth-start` authorization URL construction, callback URL parsing, one-shot localhost listener via `auth oauth-listen`, state validation, authorization-code and refresh-token request form-body construction, curl-backed exchange helpers, token response parsing, managed AuthSession persistence helpers, `auth oauth-complete`, parameterized provider adapter native OAuth login/refresh completion, `auth oauth-callback`, `auth oauth-token-request`, `auth oauth-refresh-request`, and RFC 7636 regression coverage.
- 2026-04-25: Added Gemini Google OAuth default authorization/token endpoints and default scopes for `oauth-start`, `oauth-complete`, and parameterized browser OAuth adapter completion, with Auth and CLI integration coverage.
- 2026-04-26: Added optional system-browser launch for `auth oauth-start` through `open_browser=true` while keeping the default scriptable output unchanged.
- 2026-04-26: Added `auth oauth-defaults [provider]` for scriptable provider OAuth defaults discovery, including unsupported-provider reporting.
- 2026-04-26: Added `auth oauth-login` as a single-command PKCE OAuth bridge that creates the authorization URL, optionally opens the browser, validates an explicit callback URL or captures one loopback callback, exchanges the code, persists the managed session, and supports deterministic state/verifier inputs for automation tests.
- 2026-04-26: Added repo-local OAuth defaults overrides via `runtime/auth_oauth_providers.tsv`, consumed by `auth oauth-defaults`, `auth oauth-start`, `auth oauth-login`, and `auth oauth-complete`, plus `auth oauth-config-validate` diagnostics.
- 2026-04-26: Added `auth profiles [provider]` to list persisted auth sessions with default-profile markers as a foundation for multi-account UX.
- 2026-04-26: Added `set_default=true` to login and OAuth completion commands so a successful profile creation/import can update provider defaults in the same command.
- 2026-04-23: Added explicit BrowserOAuthUnavailable handling for Gemini browser-OAuth passthrough when no Gemini CLI OAuth session can be imported.
- 2026-04-23: Added Auth tests for logout, SessionStore reload status, and missing environment credential refs.
- 2026-04-23: Added scheduler run history metadata backed by `runtime/scheduler/runs.tsv` and `schedule history`.
- 2026-04-23: Added scheduler retry/backoff fields and verified failed tasks retry before disabling.
- 2026-04-23: Added small scheduler recurrence grammar `every:<n>s|m|h|d` as an MVP alternative to full cron.
- 2026-04-25: Added five-field scheduler cron support with validation, next-run computation, persistence, CLI integration, and run/reschedule coverage.
- 2026-04-25: Added scheduler cron aliases `@hourly`, `@daily`, `@weekly`, and `@monthly` with validation, next-run computation, persistence, CLI integration, and run/reschedule coverage.
- 2026-04-25: Added scheduler `@yearly` / `@annually` aliases and cron day-of-month / day-of-week OR semantics with deterministic calendar regression coverage.
- 2026-04-23: Added scheduler regression coverage for disabled tasks and missed interval tasks.
- 2026-04-23: Added controllable Codex/Claude CLI session fixture tests for auth probe/import success and unavailable-session failure.
- 2026-04-23: Added `schedule daemon` foreground loop as the long-running scheduler wrapper, reusing the `tick` execution path.
- 2026-04-23: Added `missed_run_policy=run-once|skip` for interval tasks and covered skip semantics.
- 2026-04-23: Split Router internals into SkillRouter, AgentRouter, and WorkflowRouter while preserving public selection behavior.
- 2026-04-23: Added automatic SubagentManager candidate selection by healthy capability match, historical stats, and lessons.
- 2026-04-23: Added WorkspaceSession abstraction for opening, using, and closing session-capable agent adapters within a workspace.
- 2026-04-23: Added SubagentManager parallel concurrency limits and estimated-cost budget checks with memory cost stats.
- 2026-04-25: Added deterministic SubagentManager role assignment via inferred capabilities or `roles=agent:role`, role-scoped subtask objectives, role summaries in output, and CLI/unit coverage.
- 2026-04-23: Added repo-local external CLI spec loading from `runtime/cli_specs/*.tsv` and verified dynamic skill registration.
- 2026-04-24: Made the external CLI spec TSV parser preserve empty fields so optional empty columns no longer shift later fields, with regression coverage.
- 2026-04-24: Added `agentos cli-specs validate` and external CLI spec load diagnostics for scriptable validation of repo-local CLI specs.
- 2026-04-23: Added built-in `jq_transform` CLI skill backed by jq and covered it with a controllable CLI fixture.
- 2026-04-23: Rechecked docs task completion against code/tests and corrected stale Roadmap, architecture, agent-system, and audit-status entries.
- 2026-04-23: Added CLI sensitive argument redaction for command display/stdout/stderr and covered it with a smoke test.
- 2026-04-23: Shared sensitive-value redaction between CLI and AuditLogger and added audit log redaction coverage.
- 2026-04-23: Enforced `idempotency_key` for non-idempotent filesystem-write skills and made Scheduler assign per-run keys automatically.
- 2026-04-23: Added explicit `approval_id` hooks for high-risk and unknown-risk skill/agent policy decisions.
- 2026-04-23: Added per-task `permission_grants` enforcement for Skill permissions and `agent.invoke`, with CLI/Scheduler/Subagent option support.
- 2026-04-25: Added persistent `RoleCatalog` backed by `runtime/trust/roles.tsv`, role/user assignment and removal CLI commands, automatic user-assignment cleanup when roles are removed, and PolicyEngine fallback to user-role permissions when per-task `permission_grants` are absent.
- 2026-04-25: Added persisted pairing invites backed by `runtime/trust/invites.tsv`, with `trust invite-create`, `trust invite-accept`, active invite listing, TTL validation, one-time consumption, permission validation, audit coverage, manifest registration, and CLI/unit tests.
- 2026-04-25: Added paired-device lifecycle metadata to `runtime/trust/allowlist.tsv`, plus `trust device-label`, `trust device-seen`, and `trust unblock` commands with audit and CLI/unit coverage.
- 2026-04-26: Added `trust device-show` for scriptable paired-device inspection with lifecycle metadata, explicit not-found diagnostics, and CLI integration coverage.
- 2026-04-25: Added persistent `ApprovalStore` backed by `runtime/trust/approvals.tsv`, CLI approval request/approve/revoke/list commands, storage manifest registration, PolicyEngine approved-state enforcement for high-risk CLI runtime execution, and CLI/unit coverage.
- 2026-04-23: Added Plugin Host with repo-local plugin specs, PluginSkillInvoker, one-shot stdio-json-v0 execution, and smoke coverage.
- 2026-04-25: Added repo-local JSON plugin manifests in `runtime/plugin_specs/*.json`, reusing plugin.v1 validation, diagnostics, duplicate detection, and one-shot stdio-json-v0 execution.
- 2026-04-25: Added optional plugin `health_args_template` / `health_timeout_ms` probes for TSV and JSON manifests, wired into `agentos plugins health` with success and failure coverage.
- 2026-04-25: Added plugin manifest diagnostics rejecting `health_args_template` placeholders that require runtime input arguments, with TSV and JSON regression coverage.
- 2026-04-25: Added plugin manifest `sandbox_mode=workspace|none` parsing, diagnostics, CLI listing, and runtime workspace path-argument containment.
- 2026-04-25: Added plugin manifest lifecycle fields `lifecycle_mode=oneshot|persistent`, `startup_timeout_ms`, and `idle_timeout_ms` for TSV/JSON manifests, CLI listing, validation, and regression coverage; `persistent` is currently restricted to `json-rpc-v0` while the reusable session manager remains open.
- 2026-04-26: Added runtime guardrails for `lifecycle_mode=persistent`: health and execution now report `PluginLifecycleUnavailable` instead of silently falling back to one-shot execution, and plugin skill output/capabilities include lifecycle metadata.
- 2026-04-26: Implemented persistent `json-rpc-v0` plugin sessions with stdin/stdout JSON-RPC round-trips, request IDs, timeout handling, stderr capture, failed-session eviction/restart, destructor shutdown, lifecycle-aware health round-trips, and persistent success/timeout/malformed-response regression coverage.
- 2026-04-26: Added persistent plugin lifecycle observability via `lifecycle_event` (`started`, `reused`, `restarted`, eviction variants) in run/skill results, plus crash-after-response restart coverage.
- 2026-04-26: Added `PluginHost` active-session counting, manual session close, `idle_timeout_ms` restart behavior, and regression coverage for session close and idle restart.
- 2026-04-26: Added `PluginHostOptions.max_persistent_sessions` with LRU eviction for persistent JSON-RPC plugin sessions and regression coverage for pool eviction/restart behavior.
- 2026-04-26: Added `agentos plugins inspect name=<plugin> [health=true]` for scriptable manifest inspection plus optional single-plugin health probes, including protocol, permissions, arguments, resource limits, sandbox, lifecycle, source metadata, validity, command availability, and missing-plugin diagnostics.
- 2026-04-26: Added `agentos plugins lifecycle` for scriptable manifest-level lifecycle summaries, including oneshot/persistent counts and default persistent session cap.
- 2026-04-26: Added workspace-configurable persistent plugin session caps through `runtime/plugin_host.tsv`, registered that file in `runtime/storage_manifest.tsv`, and surfaced invalid plugin host config diagnostics through `agentos plugins lifecycle`.
- 2026-04-26: Added per-plugin `PluginSpec.pool_size` cap with LRU eviction scoped to the same plugin name (capped by global `max_persistent_sessions`), TSV/JSON manifest parsing, and `agentos plugins lifecycle` / `plugins inspect` `pool_size` reporting.
- 2026-04-26: Added runtime plugin session admin commands `agentos plugins sessions`, `agentos plugins session-restart name=<plugin>`, and `agentos plugins session-close name=<plugin>`, exposing `PluginHost::list_sessions/close_sessions_for_plugin/restart_sessions_for_plugin`, plus pool/admin and CLI integration regression coverage.
- 2026-04-26: Added `trust role-show`, `trust user-role-show`, and `trust approval-show` for scriptable single-record Trust/Policy admin inspection with not-found diagnostics and CLI integration coverage.
- 2026-04-25: Extended `agentos plugins`, `agentos plugins validate`, and `agentos plugins health` loaded-spec output with source file and line metadata.
- 2026-04-25: Required plugin manifests to declare `process.spawn`, moving missing process-spawn permission from implicit runtime behavior into manifest diagnostics.
- 2026-04-25: Enforced JSON-object-shaped stdout for successful `stdio-json-v0` plugin runs and added invalid-output regression coverage.
- 2026-04-25: Added `json-rpc-v0` plugin protocol support with JSON-RPC 2.0 response validation, result-object extraction into `plugin_output`, output-schema validation against `result`, and error-response rejection coverage.
- 2026-04-25: Added structured `plugin_output` embedding to PluginSkillInvoker results while retaining raw stdout/stderr metadata.
- 2026-04-25: Added `output_schema_json.required` validation for successful `stdio-json-v0` plugin output.
- 2026-04-25: Added `output_schema_json.properties.*.type` validation for successful `stdio-json-v0` plugin output.
- 2026-04-25: Added string `const` and `enum` validation for successful `stdio-json-v0` plugin output.
- 2026-04-25: Added string `minLength`, `maxLength`, and `pattern` validation for successful `stdio-json-v0` plugin output.
- 2026-04-25: Added numeric `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, and `multipleOf` validation for successful `stdio-json-v0` plugin output.
- 2026-04-25: Added `output_schema_json.additionalProperties:false` validation for successful `stdio-json-v0` plugin output.
- 2026-04-25: Added `output_schema_json.allOf` / `anyOf` / `oneOf` required-branch validation for successful plugin output.
- 2026-04-25: Added `output_schema_json.propertyNames`, `minProperties`, and `maxProperties` validation for successful plugin output.
- 2026-04-25: Added `output_schema_json.dependentRequired`, legacy array-valued `dependencies`, and object-level `not.required` validation for successful plugin output.
- 2026-04-25: Added basic `output_schema_json.if` / `then` / `else` required-branch validation for successful plugin output.
- 2026-04-23: Added plugin.v1 manifest version and unsupported plugin protocol/version filtering.
- 2026-04-23: Added shared plugin spec health validation for loader, runtime rejection, and PluginSkillInvoker health.
- 2026-04-24: Added structured plugin health reporting and an `agentos plugins` command with CLI integration coverage.
- 2026-04-24: Added `agentos plugins health` for scriptable plugin health checks with failing exit status for unhealthy loaded plugins.
- 2026-04-24: Added plugin spec load diagnostics so skipped plugin specs report source file, line number, and reason through the loader and plugin health CLI.
- 2026-04-24: Added `agentos plugins validate` for scriptable plugin manifest validation that reports skipped-spec diagnostics without requiring plugin binaries to be installed.
- 2026-04-24: Added startup `config_diagnostic` audit events for skipped external CLI specs and plugin specs so invalid repo-local capability declarations are visible outside validation commands.
- 2026-04-24: Tightened external CLI and plugin spec parsing so invalid numeric/resource fields, plus plugin boolean fields, become skipped-spec diagnostics instead of silently falling back to defaults.
- 2026-04-24: Added spec validation bounds so external CLI and plugin `timeout_ms` must be positive and integer resource-limit fields must be non-negative, preventing negative timeout/resource settings from reaching process execution.
- 2026-04-24: Added external CLI and plugin spec diagnostics for `required_args` that are not referenced by `args_template` placeholders, while allowing reserved `cwd` to be required.
- 2026-04-24: Added lightweight external CLI and plugin spec diagnostics for `input_schema_json` / `output_schema_json` values that are not JSON-object-shaped, avoiding silent schema validation disablement from malformed schema fields.
- 2026-04-24: Added external CLI `parse_mode` whitelist diagnostics so repo-local CLI specs must declare one of `text`, `json`, or `json_lines`.
- 2026-04-24: Added external CLI and plugin spec diagnostics for unsupported `risk_level` values and unknown permissions, moving repo-local capability policy mistakes from runtime denial to validation.
- 2026-04-24: Added duplicate-name diagnostics for external CLI and plugin specs so later repo-local declarations no longer silently overwrite earlier registered skills.
- 2026-04-24: Preserved source file/line metadata on loaded external CLI and plugin specs and skipped startup registration when a repo-local spec name conflicts with an already registered built-in skill, recording the conflict in audit.
- 2026-04-24: Extended `agentos cli-specs validate`, `agentos plugins validate`, and `agentos plugins health` to report name conflicts against built-in skills and external CLI specs without falsely flagging already-registered valid specs as self-conflicts.
- 2026-04-25: Added CLI integration regression coverage proving plugin specs that share a name with a valid external CLI spec fail `plugins validate` and are audited during startup.
- 2026-04-25: Extended CLI/plugin validate output with `conflicting_cli_spec` / `conflicting_plugin` source file and line diagnostics for registered-skill name conflicts.
- 2026-04-25: Extracted shared external spec parsing helpers for TSV splitting, strict numeric parsing, JSON-object shape checks, and string joining to keep CLI and Plugin loaders consistent.
- 2026-04-25: Added `agentos_spec_parsing_tests` for direct coverage of shared spec parsing helpers and increased local verification to 10 CTest targets.
- 2026-04-25: Added authenticated Gemini `generateContent` agent adapter backed by existing auth sessions/default profiles and `CliHost` curl execution, plus `agentos_agent_provider_tests`; local verification now passes 11 CTest targets.
- 2026-04-25: Added Gemini CLI browser-OAuth passthrough import (`auth login gemini mode=browser_oauth`) and routed external Gemini sessions through `gemini -p`, plus empty-stdin CLI execution to avoid headless Gemini hangs.
- 2026-04-25: Added Gemini `cloud_adc` login via Google Application Default Credentials plus `gcloud auth application-default print-access-token`, and routed Gemini REST calls through the minted bearer token.
- 2026-04-25: Added model-selection propagation from `agentos run ... model=<name>` into agent constraints, covered Gemini CLI OAuth routing with `gemini-3.1-pro` shorthand normalized to the official `gemini-3.1-pro-preview` model id, and verified a real Gemini 3.1 Pro Preview smoke through the imported CLI OAuth session.
- 2026-04-25: Added `qwen` agent adapter backed by Qwen API-key auth sessions and Alibaba Cloud Model Studio OpenAI-compatible Chat Completions, with endpoint/auth/model/summary/redaction coverage in `agentos_agent_provider_tests`.
- 2026-04-25: Normalized agent structured output with a top-level `content` field for Codex CLI, Gemini, Anthropic, and Qwen provider adapters.
- 2026-04-25: Replaced the production `mock_planner` naming/path with `local_planner`, a local deterministic planning adapter that tracks sessions, rejects closed sessions, emits structured plan steps, and records pre-execution cancellation.
- 2026-04-25: Added deterministic per-agent subtask objectives for SubagentManager through `subtasks=role_or_agent=objective;...` and `subtask_<agent|role>=...`, with orchestration regression coverage.
- 2026-04-25: Added `auto_decompose=true` SubagentManager path that invokes a decomposition-capable planner, extracts `plan_steps[].action`, maps generated actions into deterministic subtask objectives, exposes `decomposition_agent` in output, and covers unit/CLI flows.
- 2026-04-25: Added subagent step-level structured output retention and top-level `agent_outputs` aggregation so provider fields such as content/model/profile/auth_source survive orchestration beyond plain summaries.
- 2026-04-24: Made the plugin TSV manifest parser preserve empty fields so optional empty columns no longer shift later fields, with regression coverage.
- 2026-04-23: Added required input schema validation before skill execution with smoke coverage.
- 2026-04-24: Extended skill input schema validation to check basic `properties.*.type` scalar types (`string`, `number`, `integer`, `boolean`) before skill execution, with regression coverage.
- 2026-04-24: Extended skill input schema validation to check `enum`, `minLength`, `maxLength`, `minimum`, and `maximum` constraints before skill execution, with regression coverage.
- 2026-04-24: Extended skill input schema validation to check string `pattern` constraints before skill execution, with regression coverage.
- 2026-04-24: Extended skill input schema validation to check numeric `multipleOf` constraints before skill execution, with regression coverage.
- 2026-04-24: Extended skill input schema validation to check numeric `exclusiveMinimum` and `exclusiveMaximum` constraints before skill execution, with regression coverage.
- 2026-04-24: Extended skill input schema validation to check string `const` constraints before skill execution, with regression coverage.
- 2026-04-24: Extended skill input schema validation to reject undeclared inputs when `additionalProperties:false` is declared, with regression coverage.
- 2026-04-25: Extended skill input schema validation to check `dependentRequired` and legacy array-valued `dependencies` before skill execution, with regression coverage.
- 2026-04-25: Extended skill input schema validation to check object-level `minProperties` and `maxProperties` before skill execution, with regression coverage.
- 2026-04-25: Extended skill input schema validation to check object-level `not.required` before skill execution, with regression coverage.
- 2026-04-25: Extended skill input schema validation to check `propertyNames` constraints before skill execution, including pattern and length coverage.
- 2026-04-25: Extended skill input schema validation to check basic `if` / `then` / `else` conditional required fields before skill execution, with regression coverage.
- 2026-04-25: Extended skill input schema validation to check object-level `allOf` / `anyOf` / `oneOf` required-branch constraints before skill execution, with regression coverage.
- 2026-04-23: Added atomic file replacement for full-file TSV stores and covered the helper with a smoke test.
- 2026-04-23: Added a single-writer lock guard to atomic full-file TSV writes.
- 2026-04-23: Added `runtime/storage_manifest.tsv` metadata with a storage status command and smoke coverage.
- 2026-04-23: Added explicit `storage migrate` support for manifest/version normalization with smoke coverage.
- 2026-04-23: Added manifest-based runtime state export and smoke coverage.
- 2026-04-23: Added manifest-based runtime state import and smoke coverage.
- 2026-04-23: Added locked append-only file helper and moved audit/task/step/scheduler history appends onto it.
- 2026-04-25: Added append-intent recovery for append-only logs so interrupted writes can be replayed on the next append without duplicating already-applied lines.
- 2026-04-23: Extended storage compaction to include malformed-line cleanup for `audit.log`.
- 2026-04-24: Recorded an explicit TSV MVP storage decision in code/docs and surfaced it through `storage status`.
- 2026-04-25: Extended `storage status` with per-component file existence, byte size, and line-count inspection for runtime diagnostics.
- 2026-04-26: Added transactional multi-file storage writes with prepare/commit/recover markers, `storage recover`, and recovery coverage for committed replay plus uncommitted rollback.
- 2026-04-26: Hardened `storage recover` so corrupt committed transaction state is counted via `failed=`, cleaned up, and does not block later valid committed replays or partially apply missing staged payloads.
- 2026-04-26: Added same-process concurrent writer coverage for append logs and atomic replace storage helpers.
- 2026-04-26: Expanded the storage policy into decision record `ADR-STORAGE-001`, surfaced through `storage status`, with deferred SQLite target, StorageBackend migration boundary, required backend capabilities, and TSV import compatibility contract.
- 2026-04-26: Added `storage verify [src=<directory>] [strict=true]` for scriptable manifest-managed file completeness checks before backup/export/import workflows.
- 2026-04-26: Added pre-import backups for `storage import`, with `backed_up_files` and `backup` path reporting for rollback visibility.
- 2026-04-26: Added `storage backups` to list pre-import backup directories with file/byte counts and an aggregate summary.
- 2026-04-26: Added `storage restore-backup name=<backup_name>` to restore pre-import backups while creating a fresh backup of overwritten current files.
- 2026-04-26: Added mixed audit recovery coverage for global events, task-scoped preserved policy events, orphan task events, scheduler_run replacement, and untimed global events; audit recovery now merges individual timed task lifecycle lines with scheduler chunks instead of emitting each recovered task as an indivisible bundle.
- 2026-04-26: Added normalized `agent_result.v1` output for Codex CLI, Gemini, Anthropic, Qwen, and local_planner adapters, plus SubagentManager aggregation of normalized summary/content/model/artifacts/metrics/tool_calls/provider metadata while preserving raw provider output.
- 2026-04-26: Validated the external Opus completion/design review, documented the scriptable-OAuth and idempotency-key semantics more explicitly, and added auto_decompose negative-path coverage for planner failure and invalid planner output.
- 2026-04-26: Added task fingerprint matching to ExecutionCache so idempotency replays require both `idempotency_key` and matching task inputs/context, while retaining legacy cache read compatibility.
- 2026-04-26: Synchronized `completion_review.md` with the completed CLI/plugin refactors, persistent plugin lifecycle, OAuth refresh implementation, and ExecutionCache fingerprint semantics.
- 2026-04-26: Moved lesson-derived PolicyDenied hints into shared `memory/lesson_hints` and reused them from AgentLoop and SubagentManager policy paths.
- 2026-04-26: Split the combined router component implementation into separate `skill_router.cpp`, `agent_router.cpp`, and `workflow_router.cpp` files while preserving existing router behavior.
- 2026-04-26: Replaced PolicyEngine constructor overloads with a single `PolicyEngineDependencies` constructor and migrated runtime/tests to the explicit dependency object.
- 2026-04-26: Added shared `core/execution/task_lifecycle` helpers and routed AgentLoop/SubagentManager step recording plus task finalization through them.
- 2026-04-26: Extracted the `agentos storage` command group from `main.cpp` into `src/cli/storage_commands.*`, preserving storage status/migrate/export/import/recover/compact behavior.
- 2026-04-26: Extracted the `agentos schedule` command group from `main.cpp` into `src/cli/schedule_commands.*`, preserving add/list/history/run-due/tick/daemon behavior.
- 2026-04-26: Extracted the `agentos subagents` command group from `main.cpp` into `src/cli/subagents_commands.*`, preserving explicit/automatic/auto-decompose subagent execution behavior.
- 2026-04-26: Extracted the `agentos agents` listing command from `main.cpp` into `src/cli/agents_commands.*`.
- 2026-04-26: Extracted the `agentos cli-specs` command group from `main.cpp` into `src/cli/cli_specs_commands.*`, preserving external CLI spec listing, validation, skipped-spec diagnostics, and registered-skill conflict diagnostics.
- 2026-04-26: Extracted the `agentos plugins` command group from `main.cpp` into `src/cli/plugins_commands.*`, preserving plugin listing, validation, health checks, skipped-spec diagnostics, and external CLI/builtin name-conflict diagnostics.
- 2026-04-26: Extracted the `agentos trust` command group from `main.cpp` into `src/cli/trust_commands.*`, preserving identity, invite, pairing, role, approval, and device lifecycle commands plus audit events.
- 2026-04-26: Extracted the `agentos memory` command group from `main.cpp` into `src/cli/memory_commands.*`, preserving memory summary/stats, lessons, workflow promotion, stored workflow filtering, update/clone/remove, validation, and applicability explanation behavior.
- 2026-04-26: Extracted the `agentos auth` command group from `main.cpp` into `src/cli/auth_commands.*`, preserving provider listing, credential-store status, status/login/refresh/probe/logout/default-profile, OAuth PKCE start/callback/listen/complete, and token request helpers.
- 2026-04-26: Started decomposing `plugin_host.cpp` by extracting shared PluginSpec support validation / PluginSpec-to-CliSpec conversion into `plugin_spec_utils.*` and moving `PluginSkillInvoker` into `plugin_skill_invoker.cpp`.
- 2026-04-26: Continued `plugin_host.cpp` decomposition by extracting plugin workspace sandbox path-argument containment into `plugin_sandbox.*`.
- 2026-04-26: Continued `plugin_host.cpp` decomposition by extracting shared plugin JSON helpers, manifest loader, output schema validator, and JSON-RPC request/response helpers into dedicated plugin modules.
- 2026-04-26: Finished `plugin_host.cpp` decomposition by extracting persistent JSON-RPC session lifecycle, health checks, and PluginHost runtime execution into dedicated plugin modules.
- 2026-04-24: Extended `storage migrate` to move legacy root-level storage files into `runtime/`, normalize known legacy memory/workflow/scheduler schemas, and report normalized targets.
- 2026-04-24: Extended `storage compact target=audit` to rebuild task lifecycle audit entries from `runtime/memory` even when `audit.log` is missing, preserve non-task audit events, reinsert task-scoped preserved events such as `policy`, preserve orphan task-scoped preserved events for tasks absent from memory logs, reuse prior lifecycle timestamps, merge task bundles plus timestamped preserved task events into stable chronological order, and place untimed global preserved events after timed chunks.
- 2026-04-25: Extended `storage compact target=audit` to rebuild `scheduler_run` audit events from `runtime/scheduler/runs.tsv`, replacing stale scheduler-run audit rows while preserving unrelated global audit events and 64-bit epoch-millisecond fields.
- 2026-04-24: Added Windows Job Object resource controls for CLI skills through `CliSpec.max_processes` / `memory_limit_bytes`, plus smoke coverage for enforced process-count limits.
- 2026-04-24: Added GitHub Actions CI for Windows + Ubuntu configure/build/test and documented matching local CI commands in `README.md`.
- 2026-04-24: Added negative-path smoke coverage for remote tasks without TrustPolicy, blocked remote peers, invalid scheduler `missed_run_policy` normalization, and auth missing-session / unregistered-provider failures.
- 2026-04-24: Added `agentos_cli_integration_tests` to exercise real CLI command groups (`agents`, `memory`, `storage`, `trust`, `schedule`, `subagents`, `auth`) through the built executable.
- 2026-04-24: Started splitting `core_smoke_tests.cpp` by adding `agentos_storage_tests` for storage/atomic-file/compaction coverage while keeping the original smoke suite intact.
- 2026-04-24: Continued the test split with `agentos_auth_tests`, moving auth/session/provider fixture coverage into a dedicated test target while keeping the original smoke suite intact.
- 2026-04-24: Continued the test split with `agentos_policy_trust_tests`, moving policy/permission/trust/pairing coverage into a dedicated test target while keeping the original smoke suite intact.
- 2026-04-24: Continued the test split with `agentos_scheduler_tests`, moving scheduler due-run/retry/missed-run coverage into a dedicated test target while keeping the original smoke suite intact.
- 2026-04-24: Continued the test split with `agentos_workflow_router_tests`, moving workflow execution, router preference, lesson-aware routing, execution-cache, and workflow-store persistence coverage into a dedicated test target while keeping the original smoke suite intact.
- 2026-04-24: De-duplicated `agentos_smoke_tests` by removing storage/auth/policy-trust/scheduler/workflow-router invocations now covered by dedicated test targets, leaving smoke focused on file-skill, subagent, workspace-session, CLI, and plugin paths.
- 2026-04-24: Continued the test split with `agentos_subagent_session_tests`, moving subagent orchestration and workspace-session coverage into a dedicated test target, and narrowed `agentos_smoke_tests` to file-skill, CLI, and plugin paths.
- 2026-04-24: Continued the test split with `agentos_cli_plugin_tests`, moving CLI host, external CLI spec, plugin host, and jq fixture coverage into a dedicated test target, and narrowed `agentos_smoke_tests` to file-skill and policy-smoke coverage.
- 2026-04-24: Completed the smoke-test split by replacing `agentos_smoke_tests` with `agentos_file_skill_policy_tests`, deleting the old monolithic `core_smoke_tests.cpp`, and keeping coverage in dedicated module-level targets only.
- 2026-04-24: Added shared `tests/test_command_fixtures.hpp` helpers for cross-platform env overrides, PATH injection, and CLI fixture scripts, and reused them across auth and CLI/plugin tests.
- 2026-04-24: Replaced the POSIX CLI `popen` path with a `fork`/`execve` runner that preserves cwd/env/output/timeout behavior and applies `CliSpec.memory_limit_bytes` / `max_processes` through `setrlimit` where available.
- 2026-04-24: Added `CliSpec.cpu_time_limit_seconds` and `file_descriptor_limit`; CPU limits are enforced by Windows Job Objects and POSIX `RLIMIT_CPU`, and file descriptor limits are enforced on POSIX through `RLIMIT_NOFILE`.
- 2026-04-24: Extended PluginSpec parsing and PluginHost execution to pass memory/process/CPU/file-descriptor resource limits through to the underlying CliHost.
- 2026-04-24: Added WorkflowStore `input_equals` applicability conditions, router matching, CLI promotion support, persistence, and workflow/router regression coverage.
- 2026-04-24: Added WorkflowStore numeric range applicability conditions through `input_number_gte` / `input_number_lte`, including router matching, CLI promotion support, persistence, and regression coverage.
- 2026-04-24: Added WorkflowStore boolean input applicability conditions through `input_bool`, including router matching, CLI promotion support, persistence, docs, and regression coverage.
- 2026-04-25: Added WorkflowStore regex input applicability conditions through `input_regex`, including router matching, CLI promotion support, persistence, legacy compaction schema update, docs, and regression coverage.
- 2026-04-25: Added WorkflowStore composite OR applicability groups through `input_any`, including router matching, CLI promotion support, persistence, legacy compaction schema update, docs, and regression coverage.
- 2026-04-25: Added WorkflowStore nested boolean applicability expressions through `input_expr`, including `&&`, `||`, `!`, parentheses, router matching, CLI promotion support, persistence, legacy compaction schema update, docs, and regression coverage.
- 2026-04-25: Added shared workflow definition validation, wired it into `memory promote-workflow`, exposed `memory validate-workflows`, and covered invalid condition diagnostics in unit and CLI integration tests.
- 2026-04-25: Added workflow applicability explanation through `memory explain-workflow`, backed by shared evaluation helpers and CLI integration coverage for matching and non-matching inputs.
- 2026-04-25: Added `memory show-workflow` to inspect a single stored workflow definition, including steps, stats, enabled state, and applicability conditions.
- 2026-04-25: Added `memory set-workflow-enabled` and `memory remove-workflow` so stored workflow definitions can be toggled or deleted from the CLI.
- 2026-04-25: Added `memory update-workflow` for validated in-place edits to stored workflow names, trigger, steps, enabled state, and applicability conditions, including clearing list-valued conditions.
- 2026-04-25: Added `memory clone-workflow` to copy stored workflow definitions under a new name for further editing.
- 2026-04-25: Added filters to `memory stored-workflows` for enabled state, trigger task type, source, and name substring.
- 2026-04-26: Refactored `SecureTokenStore` around an `ISecureTokenBackend` abstraction and added macOS Keychain (Security framework) and Linux Secret Service (libsecret, optional CMake dependency) backends with an in-memory test backend so unit tests never touch the real keychain. `auth credential-store` now reports `windows-credential-manager` / `macos-keychain` / `linux-secret-service` / `env-ref-only` per platform.
- 2026-04-26: Broadened OAuth provider discovery: added `origin` (builtin/config/stub/none) and `note` metadata to `OAuthProviderDefaults`, registered stub entries for `openai` / `anthropic` / `qwen`, and extended `auth oauth-config-validate` with `--all` to enumerate every registered provider in one shot.
- 2026-04-26: Authored `docs/ADR-JSON-001.md` (Proposed) recommending nlohmann/json adoption with a phased migration plan. No code or build-system changes yet.
