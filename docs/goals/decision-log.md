# Goal Decision Log

Use this log for small decisions made while executing goal packets. Prefer `CONTEXT.md` for domain language. Create an ADR only when the decision is hard to reverse, surprising without context, and a real trade-off.

## Existing Decisions From Grill

- Agent calls produce **Dispatch Results**; Skills and Plugins produce **Capability Output**.
- Agent Dispatch owns policy evaluation and policy audit.
- AgentLoop and Subagent Orchestration own final task-step recording time.
- Decomposition planner calls use Agent Dispatch, while Subagent Orchestration interprets the plan.
- Route Hints live in Capability Declarations unless a separate lifecycle becomes necessary.
- Capability Contract validation checks declarations; Policy Decision handles runtime authorization.
- Runtime Store is the domain concept; StorageBackend is the implementation seam.
- Audit History is append-only evidence. Reconstruction is a recovery fallback, not semantic equivalence.
- Auth Profile is the credential/session selection concept. Avoid "account".
- Auth Login Flow obtains tokens and sessions; Credential Store owns platform backend selection.

## 2026-05-06 G001 Agent Dispatch Seam

- Added `AgentDispatchInput` and `AgentDispatchResult` as the shared boundary for agent policy evaluation, dispatch, and step candidate creation.
- Agent Dispatch records policy audit events but intentionally does not record final task steps; callers remain responsible for task lifecycle ownership.
- Direct dispatch tests live in `agentos_agent_dispatch_tests`; the existing subagent session tests remain the compatibility suite for current callers.

## 2026-05-06 G002 Agent Dispatch Callers

- `AgentLoop::run_agent_task` and `SubagentManager::run_one` now call `DispatchAgent` for agent policy evaluation, policy audit, V2 invocation preference, and legacy fallback.
- AgentLoop still records the final task step and owns `TaskRunResult` finalization; Subagent Orchestration still records returned step candidates in caller-controlled order.
- `resume_session_id` forwarding is part of Agent Dispatch input so the shared seam preserves AgentLoop V2 resume behavior.

## 2026-05-06 G003 Decomposition Planner Dispatch

- `auto_decompose=true` planner calls now go through `DispatchAgent`, including policy audit, cancellation, and V2/legacy adapter selection.
- Agent Dispatch still returns only a planner step candidate; Subagent Orchestration alone parses `plan_steps[].action` and injects subtasks.
- The decomposition planner step remains non-visible in `TaskRunResult.steps` to preserve worker step ordering and existing orchestration output shape.

## 2026-05-06 G004 REPL Development And Research Dispatch

- REPL free-form text now records a `ClassifyInteractiveRequest` route decision after slash-command handling and persists the decision under `runtime/routing`.
- `development_agent` decisions dispatch a normal `development_request` task through `AgentLoop`; the existing background job wrapper remains only REPL presentation.
- `research_agent` decisions dispatch a normal `research_request` task with network policy enabled so Capability Contract and policy checks run through the normal skill path.

## 2026-05-06 G005 Capability Contract Facade

- Added `CapabilityContractValidationResult` and diagnostics to represent declaration and input/output shape validation without moving callers yet.
- The facade wraps existing schema validation messages so current public errors stay unchanged.
- Capability Contract validation remains declaration/shape validation only; runtime authorization remains in `PolicyEngine`.

## 2026-05-06 G006 Capability Contract Callers

- AgentLoop skill input validation now goes through `ValidateCapabilityInput`, preserving existing schema failure messages.
- CLI spec and Plugin manifest loaders validate declaration-time schema/risk/permission fields via `ValidateCapabilityDeclaration`, with local adapters preserving existing CLI-visible diagnostic strings.
- Plugin output schema validation now goes through `ValidateCapabilityOutput`; runtime authorization remains in `PolicyEngine` and plugin process permission policy remains outside the Capability Contract facade.

## 2026-05-06 G007 Learned Capability Declaration

- `learn_skill` still writes learned CLI specs as TSV for compatibility, but now constructs and validates the learned Capability Declaration with `ValidateCapabilityDeclaration` before writing.
- Shared external skill reload remains in `cli/skill_reload.*`; `learn_skill` calls that helper directly and does not depend on REPL internals.
- Protected runtime skill names include `learn_skill`, `development_request`, and `research_request`; already registered names are rejected before learned specs are written.

## 2026-05-06 G008 Capability Declaration Route Hints

- Route Hints remain derived from existing Capability Declaration fields rather than a new manifest field or `runtime/skill_routes.tsv`.
- The main-agent prompt consumes each loaded `SkillManifest` description, capabilities, risk, and `input_schema_json.required` values to produce deterministic use-when and required-input hints.
- The REPL classifier may inspect loaded manifest names/availability for direct skill-use decisions, but it does not introduce a second route-hint lifecycle.

## 2026-05-06 G009 Plugin Protocol Output Normalization

- `PluginRunResult::structured_output_json` is the protocol-normalized Plugin output object: stdio JSON keeps stdout as the object, while JSON-RPC stores only the `result` object.
- `PluginHost` owns protocol parsing and output schema validation before returning successful plugin results.
- `PluginSkillInvoker` maps only normalized structured output into the Capability Output envelope field, preserving the existing Skill-facing shape without reparsing raw stdout or JSON-RPC responses.

## 2026-05-06 G010 Plugin Session Process-Pool Policy

- Plugin Session admin remains process-scoped to the current `PluginHost`; it does not manage agent Workspace Sessions or cross-process persistent capability sessions.
- Session admin outputs `scope=process persistence=none`; unsupported future scopes return scriptable `plugin_sessions_unavailable ... supported_scope=process` diagnostics.
- Persistent process pools are reported as `per_plugin_workspace_binary_lru`, with per-plugin `pool_size` still capped by `runtime/plugin_host.tsv` `max_persistent_sessions`.

## 2026-05-06 G011 Runtime Store StorageBackend Seam

- Added `StorageBackend` plus `TsvStorageBackend` as the Runtime Store backend boundary.
- The TSV adapter wraps existing atomic file, append, transaction, manifest, export/import, and migrate helpers without changing on-disk layout.
- Compaction is represented on the backend seam but remains delegated to owning stores until callers move in later goals.

## 2026-05-06 G012 Storage Commands Through StorageBackend

- Storage status, verify, export, import, migrate, recover, and compact commands now construct a `TsvStorageBackend` for backend-owned storage operations.
- Manifest diagnostics are sourced from `StorageBackend::verify_manifest`, including missing and non-regular manifest paths while preserving the existing CLI summary format.
- Runtime store compaction remains owned by the concrete stores, with `StorageBackend::compact` called as the backend seam and reporting delegated TSV compaction.

## 2026-05-06 G013 Audit History Recovery Fidelity

- Audit History remains append-only evidence; recovery-generated lifecycle entries are deterministic fallback records, not semantic equivalents of the original audit lines.
- Recovery from `runtime/memory` can rebuild task start/route/step/end shape and can preserve existing timestamps, but original-only fields such as workspace evidence and route rationale are lossy.
- Scheduler history can rebuild `scheduler_run` events, while non-task global audit events and task-scoped preserved events remain merged by timestamp where existing audit lines are still present.

## 2026-05-06 G014 Auth Login Flow Modules

- Extracted Auth Login Flow modes behind `auth_login_flow.*`; provider adapters now select modes but delegate token/session construction to the flow module.
- Login flows use `SecureTokenStore` and `SessionStore` through the existing Credential Store seams and do not select platform token backends.
- `StaticAuthProviderAdapter` remains responsible for descriptors, defaults, endpoint diagnostics, and provider probes.

## 2026-05-06 G015 Auth Profile Strategy

- Auth Profile remains the provider-specific session selection concept; `profile=` and `auth_profile=` stay compatible as task-level overrides.
- Provider defaults are persisted through `runtime/auth_profiles.tsv`; auth status, refresh, logout, OAuth, and login commands resolve omitted profiles through the default mapping.
- Multiple sessions for one provider are listed with `auth profiles [provider]`, with exactly the mapped default marked as `default=true`.

## 2026-05-06 G017 Domain Docs Maintenance

- `improve-plan.md`, `README.md`, `docs/ROADMAP.md`, `docs/ARCH_ALIGNMENT.md`, and `docs/ARCHITECTURE.md` now reflect the completed Agent Dispatch, Capability Contract, Plugin Session, Runtime Store, Audit History, Auth Login Flow, and Auth Profile goal slices.
- Documentation now uses Capability Output for normalized plugin protocol results, while `plugin_output` remains only an implementation envelope field where needed.
- Stale open-item wording for Plugin Session process-pool policy, `learn_skill`, REPL development/research dispatch, Route Hints, and the TSV-backed StorageBackend seam was removed or narrowed to remaining future work.
