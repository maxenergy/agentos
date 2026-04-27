# AgentOS Roadmap

Last synced: 2026-04-27

This roadmap reflects the current codebase state. The detailed execution checklist lives in [`../plan.md`](../plan.md).

## Status Legend

- ✅ Implemented MVP
- 🚧 Partial / in progress
- ❌ Not implemented

---

## Phase 0: Docs And Interface Freeze

Status: ✅ Implemented MVP

Delivered:

- Product, architecture, auth, skill, agent, memory, CLI, and coding guide docs
- Core C++ interface model in `src/core/models.hpp`
- Architecture alignment doc in `docs/ARCH_ALIGNMENT.md`

Remaining:

- Keep older design docs synced with implementation status
- Update `plan.md` after each milestone

---

## Phase 1: Minimal Core Runtime

Status: ✅ Implemented MVP

Delivered:

- `AgentLoop`
- `SkillRegistry`
- `AgentRegistry`
- `Router`
- Router decomposition into SkillRouter / AgentRouter / WorkflowRouter
- `PolicyEngine`
- `PermissionModel`
- `AuditLogger`
- CMake project and smoke test target

Remaining:

- Richer failure recovery and lifecycle semantics

---

## Phase 2: Builtin Skills

Status: ✅ Implemented MVP

Delivered:

- `file_read`
- `file_write`
- `file_patch`
- `http_fetch`
- `workflow_run`
- required input field validation plus `properties.*.type`, `const`, `enum`, `minLength` / `maxLength`, `pattern`, `additionalProperties:false`, `propertyNames`, `minProperties` / `maxProperties`, `minimum` / `maximum` / `exclusiveMinimum` / `exclusiveMaximum` / `multipleOf`, `dependentRequired`, legacy array-valued `dependencies`, object-level `not.required`, basic `if` / `then` / `else`, and `allOf` / `anyOf` / `oneOf` required-branch validation from skill `input_schema_json`

Remaining:

- richer JSON Schema keywords beyond required fields, scalar types, const, enum, length, pattern, property-name constraints, object property counts, `additionalProperties:false`, numeric constraints, dependent required fields, simple negated required groups, basic conditionals, and simple combinator required-branch checks

---

## Phase 3: CLI Host

Status: ✅ Implemented MVP

Delivered:

- `CliHost`
- `CliSkillInvoker`
- `rg_search`
- `git_status`
- `git_diff`
- `curl_fetch`
- `jq_transform`
- external CLI spec loader from `runtime/cli_specs/*.tsv`
- TSV external CLI spec parsing preserves empty fields so optional columns do not shift later fields
- `required_args` must be referenced by `args_template` placeholders, with reserved `cwd` allowed
- `input_schema_json` and `output_schema_json` must be JSON-object-shaped
- external CLI `parse_mode` must be one of `text`, `json`, or `json_lines`
- `risk_level` must be one of `low`, `medium`, `high`, or `critical`, and permissions must be known `PermissionModel` permissions
- plugin manifests must declare `process.spawn` because PluginHost execution always launches a process
- duplicate external CLI spec names are reported as skipped-spec diagnostics instead of silently overwriting earlier declarations
- external CLI specs keep source file/line metadata and cannot override already registered built-in skills at startup
- `agentos cli-specs validate` reports external CLI name conflicts with built-in skills
- invalid numeric/resource fields are reported as skipped-spec diagnostics instead of silently using defaults, with positive `timeout_ms` and non-negative resource-limit enforcement
- `agentos cli-specs validate` command for scriptable external CLI spec validation and skipped-spec diagnostics
- startup `config_diagnostic` audit events for skipped external CLI specs
- cwd boundary checks
- timeout
- stdout/stderr capture
- output limit
- env allowlist
- command/stdout/stderr redaction for sensitive argument values

Remaining:

- Windows file-handle limits and richer sandboxing for CLI resource controls

---

## Phase 4: Secondary Agent Adapter

Status: 🚧 Partial

Delivered:

- `IAgentAdapter`
- `local_planner` local deterministic planning adapter with session tracking and structured plan output
- `codex_cli`
- `gemini` authenticated `generateContent` adapter through existing auth sessions/default profile mapping
- `anthropic` authenticated Messages adapter plus Claude CLI passthrough execution
- `qwen` authenticated Alibaba Cloud Model Studio OpenAI-compatible Chat Completions adapter
- `openai` authenticated Chat Completions adapter targeting `api.openai.com/v1/chat/completions` (sync + V2 SSE streaming, default gpt-4o)
- per-task `profile=` / `auth_profile=` override for provider-agent auth session selection across `run`, `schedule add`, and `subagents run`
- normalized `agent_result.v1` structured output for Codex CLI, Gemini, Anthropic, OpenAI, Qwen, and local_planner, preserving raw provider output
- Agent health listing
- Router selection by health and basic historical score
- WorkspaceSession abstraction
- session-aware agent execution helper
- deterministic per-agent subtask objectives via `subtasks=role_or_agent=objective;...` or `subtask_<agent|role>=...`
- `auto_decompose=true` subagent orchestration path that calls a decomposition-capable planner, extracts `plan_steps[].action`, and maps generated actions into deterministic subtask objectives
- subagent step-level structured output/artifact retention and top-level `agent_outputs[].normalized` aggregation for normalized provider fields beyond plain summary text

Remaining:

- richer model-driven complex task decomposition

---

## Phase 5: Auth Subsystem

Status: 🚧 Partial

Delivered:

- `AuthManager`
- Provider adapters
- `SessionStore`
- `SecureTokenStore` with env-ref support and Windows Credential Manager managed-token backend
- `CredentialBroker`
- API-key profile support
- Gemini CLI browser-OAuth passthrough import
- Gemini Google ADC passthrough through `gcloud auth application-default print-access-token`
- workspace default profile mapping
- `auth profiles [provider]` session/profile discovery with default profile markers
- `set_default=true` on login / OAuth completion commands for one-step default profile selection
- Codex / Claude CLI session probes
- Codex / Claude CLI session import fixture coverage
- refresh command and adapter/store refresh path
- credential store status reports the active backend, including Windows Credential Manager or non-Windows env-ref dev fallback
- reusable OAuth PKCE start/callback scaffolding with S256 challenge generation, authorization URL construction, OAuth defaults discovery through `oauth-defaults`, repo-local OAuth defaults overrides through `runtime/auth_oauth_providers.tsv`, `oauth-config-validate` diagnostics, optional system-browser launch through `oauth-start open_browser=true`, one-shot localhost callback listener, callback URL query parsing, state validation, authorization-code and refresh-token request form-body construction, curl-backed HTTP exchange helpers, token response parsing, managed AuthSession persistence helpers, single-command `oauth-login` orchestration, scriptable `oauth-complete` orchestration, and provider adapter parameterized native OAuth login/refresh completion
- Gemini Google OAuth default authorization/token endpoints and default scopes for `oauth-defaults`, `oauth-start`, `oauth-complete`, and native browser OAuth adapter completion
- OpenAI PKCE default authorization/token endpoints (`auth.openai.com/authorize` + `/oauth/token`) and default scopes; `OpenAiAuthProviderAdapter` supports `browser_oauth` mode with refresh token
- Gemini browser-OAuth passthrough import via Gemini CLI OAuth state; unavailable imports return `BrowserOAuthUnavailable`
- `agentos auth ...` command group, including `oauth-defaults` for provider OAuth discovery, `oauth-config-validate` for repo-local OAuth defaults diagnostics, `oauth-start` for scriptable PKCE authorization URL generation, `oauth-login` for single-command start/listen/token-exchange/session persistence, `oauth-callback` for callback URL parsing/state validation, `oauth-listen` for one-shot loopback callback capture, `oauth-complete` for scriptable callback-to-session completion, `oauth-token-request` for scriptable token request body generation, and `oauth-refresh-request` for scriptable refresh body generation

Remaining:

- Additional provider-specific OAuth discovery (Anthropic remains deferred), fuller multi-provider interactive login UX
- non-Windows system credential store integration
- cloud credential modes

---

## Phase 6: Memory And Workflow Learning

Status: 🚧 Partial

Delivered:

- Task log
- Step log
- Skill stats
- Agent stats
- LessonStore
- lesson-driven routing hints
- lesson-driven policy hints
- Workflow candidate generation
- Workflow scoring
- durable WorkflowStore skeleton
- workflow promotion command
- workflow definition validation and `agentos memory validate-workflows`
- filtered workflow listing through `agentos memory stored-workflows`
- single workflow inspection through `agentos memory show-workflow`
- workflow cloning through `agentos memory clone-workflow`
- workflow definition rename, condition clearing, and edits through `agentos memory update-workflow`
- workflow enable/disable and removal through `agentos memory set-workflow-enabled` / `remove-workflow`
- workflow applicability explanation through `agentos memory explain-workflow`
- stored workflow execution through `workflow_run`
- auto workflow selection by Router
- `required_inputs`, `input_equals`, numeric range, boolean, regex, `input_any` composite OR, and `input_expr` nested boolean workflow applicability checks
- append-only log append-intent recovery for interrupted audit/task/step/scheduler history writes
- transactional multi-file storage helper with prepare/commit/recover markers and `agentos storage recover`, including corrupt committed transaction isolation with `failed=` reporting
- storage reliability tests cover append-intent recovery plus same-process concurrent append and atomic replace writers
- `agentos storage verify [src=<directory>] [strict=true]` for manifest-managed file completeness diagnostics before export/import workflows
- `agentos storage import src=...` creates a manifest-managed pre-import backup under `runtime/.import_backups/` and reports `backup=...`
- `agentos storage backups` lists pre-import backup directories with relative paths, file counts, byte counts, and an aggregate summary
- `agentos storage restore-backup name=<backup_name>` restores a listed pre-import backup by directory name and creates a fresh backup of overwritten current files
- storage policy decision record `ADR-STORAGE-001`, surfaced through `agentos storage status`, defining the deferred SQLite target, StorageBackend migration boundary, required backend capabilities, and TSV import compatibility contract
- mixed audit recovery coverage for global events, task-scoped preserved policy events, orphan task events, scheduler_run replacement, and untimed global events; recovered task lifecycle lines now merge with scheduler chunks by timestamp
- `agentos memory ...` command group

Remaining:

- richer workflow definition editing UX beyond the current required / exact / numeric / boolean / regex / composite OR / nested boolean input matches, show/explain commands, cloning, in-place updates, enable toggles, and removal
- SQLite implementation beyond the current `ADR-STORAGE-001` boundary decision
- fuller audit recovery beyond current task lifecycle, scheduler-run rebuilds, and mixed timeline preservation
- broader cross-format storage migration beyond the current manifest normalization, legacy path migration, and known TSV schema normalization

---

## Phase 7: Identity, Trust, And Policy

Status: ✅ Implemented MVP

Delivered:

- `IdentityManager`
- `PairingManager`
- `PairingInviteStore` backed by `runtime/trust/invites.tsv`
- `AllowlistStore`
- `TrustPolicy`
- one-time pairing invite handshake with TTL, active invite listing, accept-to-pair, permission validation, and audit events
- paired-device lifecycle metadata with paired/last_seen timestamps plus label update, seen update, block, unblock, and remove commands
- single paired-device inspection through `agentos trust device-show identity=<id> device=<id>`
- remote trigger identity/device fields
- pairing-required remote task policy
- per-task `permission_grants` enforcement on top of `PermissionModel`
- idempotency key enforcement for side-effecting filesystem-write skills
- explicit `approval_id` hook for high-risk or unknown-risk operations
- persistent `ApprovalStore` backed by `runtime/trust/approvals.tsv`; high-risk CLI runtime policy requires an approved `approval_id`
- approval request, approve, revoke, and list commands
- trust mutation audit events
- audit redaction for sensitive task values
- `agentos trust ...` command group

Remaining:

- richer admin UX beyond persisted RoleCatalog and ApprovalStore grants/show/removal

---

## Phase 8: Scheduler

Status: 🚧 Partial

Delivered:

- `Scheduler`
- persisted `ScheduledTask`
- one-shot tasks
- interval tasks
- manual `schedule run-due`
- foreground `schedule tick` loop
- foreground `schedule daemon` loop
- scheduler-specific execution metadata
- retry/backoff fields
- small recurrence grammar (`every:<n>s|m|h|d`)
- five-field cron expressions and @hourly/@daily/@weekly/@monthly/@yearly/@annually aliases with validation and persistence
- day-of-month / day-of-week OR semantics when both cron fields are restricted
- configurable missed-run policy (`run-once` or `skip`)
- disabled-task and missed-interval regression coverage

Remaining:

- deeper calendar edge-case hardening if product requirements need timezone/DST-specific guarantees

---

## Phase 9: Subagent Orchestration

Status: 🚧 Partial

Delivered:

- `SubagentManager`
- explicit agent list orchestration
- sequential mode
- parallel mode
- automatic subagent candidate selection
- deterministic role assignment from capabilities or `roles=agent:role`
- role-scoped subtask objectives
- WorkspaceSession abstraction
- parallel concurrency limits
- estimated-cost budget checks
- shared Policy / Audit / Memory path
- `agentos subagents run ...`

Remaining:

- task decomposition
- richer model-driven complex task decomposition beyond the current `auto_decompose=true` plan-step mapping

---

## Phase 10: Plugin And Ecosystem

Status: 🚧 Partial

Delivered:

- Plugin Host
- repo-local `runtime/plugin_specs/*.tsv` and `runtime/plugin_specs/*.json` loaders
- `plugin.v1` TSV/JSON manifest version with unsupported-version/protocol filtering
- `PluginSkillInvoker` registration through `SkillRegistry`
- `stdio-json-v0` and `json-rpc-v0` process protocols backed by existing cwd/timeout/env/output controls
- successful `stdio-json-v0` plugin runs must emit JSON-object-shaped stdout
- successful `json-rpc-v0` plugin runs must emit JSON-RPC 2.0 responses with JSON-object `result`; error responses are rejected
- successful plugin stdout is embedded as structured `plugin_output` while raw stdout/stderr are retained
- `output_schema_json.required` is validated against successful plugin output
- `output_schema_json.properties.*.type` is validated for basic plugin output field types
- string `const` and `enum` constraints are validated for plugin output fields
- string length and pattern constraints are validated for plugin output fields
- `additionalProperties:false` is validated for plugin output fields
- `propertyNames`, `minProperties`, and `maxProperties` are validated for plugin output fields
- `dependentRequired`, legacy array-valued `dependencies`, and object-level `not.required` are validated for plugin output fields
- basic `if` / `then` / `else` required-branch constraints are validated for plugin output fields
- numeric range and multipleOf constraints are validated for plugin output fields
- `allOf` / `anyOf` / `oneOf` required-branch constraints are validated for plugin output fields
- plugin spec health validation for manifest version, protocol, name, binary, and command availability
- structured plugin health reporting through `CheckPluginHealth`
- `agentos plugins` command for listing loaded plugin specs and health
- `agentos plugins validate` command for scriptable plugin manifest validation without requiring plugin binaries to be installed
- `agentos plugins health` command for scriptable plugin health checks
- `agentos plugins lifecycle` command for scriptable manifest-level lifecycle summaries, oneshot/persistent counts, configured persistent session cap reporting, and plugin host config diagnostics
- `agentos plugins inspect name=<plugin> [health=true]` command for scriptable single-plugin manifest inspection and optional single-plugin health checks
- optional plugin `health_args_template` / `health_timeout_ms` probes for TSV and JSON manifests
- validation that rejects health probe templates requiring runtime input placeholders
- skipped plugin spec diagnostics with source file, line number, and reason
- `required_args` must be referenced by `args_template` placeholders, with reserved `cwd` allowed
- `input_schema_json` and `output_schema_json` must be JSON-object-shaped
- `risk_level` must be one of `low`, `medium`, `high`, or `critical`, and permissions must be known `PermissionModel` permissions
- duplicate plugin spec names are reported as skipped-spec diagnostics instead of silently overwriting earlier declarations
- plugin specs keep source file/line metadata, show it in `agentos plugins` / `validate` / `health`, and cannot override already registered built-in skills at startup
- `agentos plugins validate` / `agentos plugins health` report plugin name conflicts with built-in skills and valid external CLI specs
- invalid numeric/resource fields and boolean fields are reported as skipped-spec diagnostics instead of silently using defaults, with positive `timeout_ms` and non-negative resource-limit enforcement
- TSV plugin manifest parsing preserves empty fields so optional columns do not shift later fields
- JSON plugin manifests support string arrays, object-valued schema fields, numeric resource fields, and boolean idempotent fields
- plugin health probes execute through the same CLI host controls as one-shot plugin commands
- plugin manifest resource limit pass-through to the underlying CLI host
- plugin manifest `sandbox_mode=workspace|none`, with default workspace mode denying path-like runtime arguments that resolve outside the active workspace
- plugin manifest lifecycle fields `lifecycle_mode=oneshot|persistent`, `startup_timeout_ms`, and `idle_timeout_ms` for TSV/JSON manifests and CLI reporting; `persistent` is restricted to `json-rpc-v0`
- persistent `json-rpc-v0` plugin sessions with stdin/stdout JSON-RPC round-trips, request IDs, timeout handling, stderr capture, failed-session eviction/restart, destructor shutdown, lifecycle-aware health round-trips, lifecycle event output, active-session counting, manual close, idle-timeout restart, workspace-configurable `runtime/plugin_host.tsv` max persistent session cap with LRU eviction, and persistent success/timeout/crash-restart/malformed-response/session-close/idle-restart/pool-eviction coverage
- plugin smoke test
- shared spec parsing unit coverage

Remaining:

- richer runtime session admin UX
- deeper process-pool policy beyond session count capping

---

## Phase 11: Resident Operation Modes

Status: ✅ Implemented MVP

Delivered:

- Interactive REPL mode (`agentos interactive`) with command parsing (run/agents/skills/status/memory/schedule/help/exit)
- HTTP API Server mode (`agentos serve [port=18080] [host=127.0.0.1]`) with REST JSON endpoints
  - `GET /api/health`  — health check
  - `GET /api/skills`  — list skills
  - `GET /api/agents`  — list agents
  - `GET /api/status`  — runtime status
  - `POST /api/run`    — execute task
  - `GET /api/schedule/list` — list scheduled tasks
  - `GET /api/memory/stats`  — memory statistics
- Default no-args behavior changed from demo to interactive REPL
- cpp-httplib v0.18.3 dependency via CMake FetchContent
- CORS support for local frontend development
- Signal-based graceful shutdown for both modes

Remaining:

- WebSocket streaming for long-running tasks
- Authentication/token-based access control for HTTP API

---

## Current Priority

Current state is a runnable local MVP with resident operation modes (Interactive REPL and HTTP API server), not a production-complete system. Follow "Current Highest-Priority Gaps" and "Post-Review Development Plan" in [`../plan.md`](../plan.md), especially auth hardening, plugin long-running lifecycle, stronger storage recovery, and richer agent orchestration.
