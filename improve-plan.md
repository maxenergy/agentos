# AgentOS Architecture Improvement Plan

Last updated: 2026-05-06

This plan merges the unfinished work in `plan.md` with the five architecture
deepening opportunities identified in the architecture review. It is not a
replacement for `plan.md`; it is the execution view for turning shallow
modules into deeper modules with better locality, leverage, and test surfaces.

## Source Inputs

- `plan.md` unfinished items:
  - Anthropic public PKCE endpoints are a Provider OAuth Defaults watch item until stable customer flows exist.
  - More advanced model-driven complex task decomposition remains open.
  - Plugin process-pool policy and richer runtime session administration remain open.
  - Storage still lacks a real `StorageBackend` seam, broader cross-format migration, and full-fidelity audit/state recovery.
  - `learn_skill` builtin is planned but not complete.
  - REPL dispatch into `development_request` / `research_request` is deferred.
  - Route-hint consumption is missing for REPL dispatch; `runtime/skill_routes.tsv` should not be the default direction unless route hints need a separate lifecycle.
- Architecture review opportunities:
  1. Deepen Agent Dispatch.
  2. Deepen Capability Contract Validation.
  3. Deepen Plugin Execution Protocol.
  4. Introduce the StorageBackend seam required by ADR-STORAGE-001.
  5. Deepen Auth Login Flow.

## Priority Order

1. Agent Dispatch: it touches agent orchestration, REPL routing, future model-driven decomposition, and V2/V1 adapter behavior.
2. Capability Contract Validation: it is the shared test surface for builtin Skill input, Plugin output, learned CLI specs, permissions/risk metadata, and Route Hints.
3. Plugin Execution Protocol: it builds on Capability Contract Validation and reduces protocol leakage before deeper process-pool policy.
4. StorageBackend: it is explicitly required before any SQLite or broader cross-format migration.
5. Auth Login Flow: most infrastructure exists; remaining provider-specific work should be isolated behind login-mode modules.

## 1. Deepen Agent Dispatch

### Current Friction

`AgentLoop` and `SubagentManager` both know how to build `AgentTask`,
construct `AgentInvocation`, choose V2 vs. legacy adapters, evaluate policy,
apply lesson hints, forward cancellation, normalize cost, and record task
steps. The existing `core/execution/task_lifecycle` module is too shallow:
deleting it removes only pass-through calls, while the real complexity remains
spread across callers.

### Plan Items Merged From `plan.md`

- More advanced model-driven complex task decomposition.
- REPL dispatch into `development_request` / `research_request`.
- Restore or replace `TestInteractiveSkillRoutesUseRuntimeHints` with coverage
  that proves REPL dispatch consumes Route Hints from loaded Capability
  Declarations.

### Target Shape

Create a deep Agent dispatch module whose interface lets callers say:
"dispatch this agent target for this task with this role/context/cancel/event
callback". It returns a high-level Dispatch Result, not a bare TaskStepRecord.
Its implementation owns:

- V2 vs. legacy adapter selection.
- TaskRequest to AgentTask / AgentInvocation projection.
- Policy evaluation, lesson-policy hinting, and policy audit.
- Cancellation checkpoints.
- AgentResult to TaskStepRecord mapping.
- Usage-cost vs. legacy estimated-cost selection.
- Step candidate construction.

### Work Slices

- [ ] Extract a shared Agent dispatch module from the duplicated paths in
  `AgentLoop::run_agent_task` and `SubagentManager::run_one`.
- [ ] Move AgentInvocation construction into that module, with explicit inputs
  for top-level agent run vs. subagent role run.
- [ ] Return a Dispatch Result that carries a step candidate plus structured
  agent output needed by AgentLoop, Subagent Orchestration, and decomposition.
- [ ] Route `auto_decompose=true` planner dispatch through the same module
  instead of direct `planner->run_task(...)`.
- [ ] Wire REPL free-form classification to dispatch `development_request` and
  `research_request` through the normal Skill/Agent execution path.
- [ ] Replace the deferred `runtime/skill_routes.tsv` test with a manifest
  Route Hint test unless a separate route-hint lifecycle becomes necessary.

### Acceptance

- Agent dispatch behavior is tested through one interface for V2 success,
  legacy fallback, policy denied, cancellation, usage-cost selection, and
  subagent role context.
- AgentLoop and Subagent Orchestration own final task-step recording time, so
  parallel ordering and decomposition visibility remain caller decisions.
- AgentLoop and SubagentManager tests stop duplicating low-level adapter
  projection expectations except for integration smoke coverage.
- REPL development/research requests are observable as normal routed tasks in
  audit and memory.

## 2. Deepen Capability Contract Validation

### Current Friction

`schema_validator` has a substantial implementation, but the interface is still
shallow: callers choose between many keyword-specific functions, and input
validation returns `SchemaValidationResult` while JSON-object output validation
returns strings. This leaks validation phase and keyword ordering into callers.
At the domain level, JSON Schema is only one part of the broader Capability
Contract exposed by each Capability Declaration.

### Plan Items Merged From `plan.md`

- Richer JSON Schema keywords for Skill input and Plugin output.
- `learn_skill` malformed arguments should return `SchemaValidationFailed`.
- Capability Declaration Route Hints need validation before they drive REPL
  routing.
- `learn_skill` should create a validated Capability Declaration and trigger
  registry reload without depending on REPL internals.

### Target Shape

Create one deep Capability Contract validation module whose interface supports:

- Builtin, external, and learned Skill input validation from `StringMap`
  arguments.
- JSON-object output validation from parsed or serialized JSON.
- Manifest/spec/route metadata schema validation.
- declaration-time permissions and risk metadata validation by delegating to
  the existing permission/risk vocabulary where appropriate.
- Stable error codes and structured diagnostics.

The implementation keeps keyword traversal, input string coercion rules, and
output native JSON rules internal.

### Work Slices

- [ ] Introduce a single validation result shape used by Skill input, Plugin
  output, and metadata validation.
- [ ] Keep the existing public error strings initially, but move callers to
  the new interface.
- [ ] Add validation for Capability Declaration Route Hints used by REPL
  dispatch.
- [ ] Make `learn_skill` validate generated specs through the shared schema
  module before writing them.
- [ ] Complete `learn_skill` registration/reload through a shared capability
  reload module, not through REPL-specific code.
- [ ] Keep runtime authorization in PolicyEngine; Capability Contract
  Validation only decides whether declarations are well-formed.
- [ ] Add focused schema module tests for input coercion vs. native JSON output
  semantics.

### Acceptance

- AgentLoop, PluginHost, CLI spec loader, Plugin manifest loader, and
  `learn_skill` do not choose keyword-specific validators directly.
- Adding a schema keyword is an implementation change plus schema-module tests,
  not a caller change.

## 3. Deepen Plugin Execution Protocol

### Current Friction

Plugin protocol details leak across `PluginHost`, `plugin_execution.cpp`, and
`PluginSkillInvoker`. `PluginSkillInvoker` re-parses stdout to reconstruct
`plugin_output`, while `PluginHost` has already validated protocol output. The
`PluginSpec` interface is wide and forces callers to know manifest, lifecycle,
protocol, sandbox, resource, health, and schema details together.

### Plan Items Merged From `plan.md`

- Deeper process-pool policy.
- Richer runtime session admin UX.
- Plugin lifecycle work should be considered not production-complete until
  process-pool policy and admin boundaries are clear.

### Target Shape

Create a deep Plugin protocol module. The seam should expose protocol-normalized
execution results, not raw stdout plus protocol rules. Adapters for
`stdio-json-v0` and `json-rpc-v0` sit behind that seam, and PluginSkillInvoker
maps protocol output into Skill-facing Capability Output.

### Work Slices

- [ ] Change `PluginRunResult` to carry a normalized structured output object
  or serialized object owned by the protocol module.
- [ ] Move protocol-specific stdout / JSON-RPC result parsing out of
  `PluginSkillInvoker`; keep only the mapping from protocol output to
  Capability Output there.
- [ ] Separate manifest description from runtime process-pool policy so
  process-pool changes do not widen the Skill-facing interface.
- [ ] Define process-scope vs. future daemon-scope Plugin Session
  administration in a small Plugin Host interface with explicit
  unsupported-scope diagnostics.
- [ ] Keep Plugin Session lifecycle separate from agent workspace sessions until
  another capability has the same lifecycle semantics.
- [ ] Add tests for protocol normalization once, then keep PluginSkillInvoker
  tests focused on SkillResult mapping.

### Acceptance

- PluginSkillInvoker no longer parses plugin stdout.
- Protocol validation, schema validation, lifecycle event selection, and
  structured output construction have locality in one module.
- Skill-facing callers consume Capability Output rather than plugin protocol
  details.
- Session admin output remains scriptable and explicitly states process-scope
  limits.

## 4. Introduce StorageBackend Seam

### Current Friction

ADR-STORAGE-001 says SQLite is deferred until a `StorageBackend` interface
exists. Current storage reliability is real, but paths, TSV formats, manifest
entries, atomic writes, transactions, export/import, compaction, and migration
knowledge still live across many stores and CLI commands.
At the domain level, the thing being protected is the Runtime Store; the
StorageBackend is the implementation seam for accessing it.

### Plan Items Merged From `plan.md`

- Decide when to move beyond TSV MVP.
- Extend audit/state recovery fidelity.
- Broader cross-format migration.
- Fuller cross-process concurrency testing.
- SQLite remains deferred until the seam exists.

### Target Shape

Introduce a TSV-backed `StorageBackend` module for the Runtime Store first.
SQLite is not part of the first slice. The interface should express the
capabilities in ADR-STORAGE-001:

- atomic replace;
- append line;
- prepare/commit/recover transaction;
- manifest status and verification;
- export/import;
- migrate;
- compact;
- audit-safe diagnostics.

### Work Slices

- [ ] Define the `StorageBackend` interface and a TSV adapter that wraps the
  existing helpers.
- [ ] Move manifest-managed file diagnostics behind the backend interface.
- [ ] Route export/import/verify/migrate/compact through the backend interface.
- [ ] Add cross-process concurrency tests for the TSV adapter where feasible.
- [ ] Add Audit History recovery tests that cover full-fidelity reconstruction
  gaps and document any intentionally lossy cases.
- [ ] Treat Audit History reconstruction as a recovery fallback, not semantic
  equivalence with the original append-only evidence.
- [ ] Revisit SQLite only after the TSV adapter satisfies the interface and the
  trigger conditions in ADR-STORAGE-001 are observed.

### Acceptance

- `storage status` still reports ADR-STORAGE-001 fields.
- Existing TSV behavior is preserved through the backend tests.
- A future SQLite adapter could be added without changing storage command
  callers.
- Audit History remains special append-only evidence inside the Runtime Store;
  compaction/reconstruction tests must state what cannot be recovered.

## 5. Deepen Auth Login Flow

### Current Friction

`StaticAuthProviderAdapter::login` owns too many login-mode branches: API key,
CLI passthrough, browser OAuth, cloud ADC, provider defaults, session
persistence, and refresh. Provider-specific behavior is mixed with login-mode
orchestration, so adding provider flows increases the interface burden on
adapters.

### Plan Items Merged From `plan.md`

- Track Anthropic public PKCE availability; promote defaults only when stable
  customer flows exist.
- Broader provider-specific OAuth discovery.
- Fuller multi-provider product login UX.
- Full Auth Profile strategy.
- Provider endpoint status remains `deferred` for providers without stable
  public PKCE endpoints.

### Target Shape

Make Auth login modes deep modules:

- API key env-ref login.
- CLI session passthrough.
- Browser OAuth PKCE.
- Cloud ADC.
- Refresh.
- Auth Profile selection.

Provider adapters supply descriptors, provider-specific probes, and provider
defaults. Login-mode modules own orchestration and session persistence requests;
Credential Store owns token backend selection and platform fallback behavior.

### Work Slices

- [ ] Extract login-mode implementations from `StaticAuthProviderAdapter`.
- [ ] Keep provider adapters focused on descriptors, defaults, and probes.
- [ ] Keep Credential Store backend selection outside Auth Login Flow modules;
  login modes should call token storage through the existing store seam.
- [ ] Add an Auth Profile selection model that works across API key,
  OAuth, CLI passthrough, and cloud ADC sessions.
- [ ] Keep Anthropic PKCE as `deferred` until stable public endpoints exist;
  when they do, promote Provider OAuth Defaults from stub to builtin and add
  interoperability tests in the Browser OAuth module.
- [ ] Add tests that exercise each login mode through fixture providers and
  stores, not through provider-specific branches.

### Acceptance

- Adding or updating a provider OAuth endpoint does not require editing the
  generic login branch structure.
- `auth oauth-defaults`, `auth oauth-config-validate --all`, and
  `auth login-interactive` keep machine-readable endpoint status.
- Multi-account behavior is documented and covered by reload/status/default
- Auth Profile behavior is documented and covered by reload/status/default
  profile tests.

## Cross-Cutting Documentation Tasks

- [x] Create `CONTEXT.md` for AgentOS domain language so future architecture
  reviews use consistent terms for Agent Dispatch, Capability Declaration,
  Capability Contract, Capability Output, Plugin Session, Runtime Store, Audit
  History, Auth Profile, Auth Login Flow, and Credential Store.
- [ ] Keep `CONTEXT.md` updated when a plan term is accepted, rejected, or
  renamed during implementation.
- [ ] Prefer `CONTEXT.md` terms in new module names, test names, and docs unless
  an existing implementation convention requires a lower-level name.
- [ ] Flag implementation-only terms such as `StorageBackend` and
  `schema_validator` as seams, not domain language.
- [ ] Keep `docs/ARCH_ALIGNMENT.md`, `README.md`, and `plan.md` synchronized
  when any item here lands.
- [ ] When rejecting a deepening candidate for a durable reason, record it as
  an ADR so future reviews do not re-suggest it.

## Suggested First PR

Start with Agent Dispatch because it unlocks several visible unfinished items,
but keep the first PR focused on the seam rather than REPL behavior:

1. Add the shared Agent dispatch module.
2. Move AgentLoop agent target execution onto it.
3. Move SubagentManager single-agent execution onto it.
4. Route decomposition planner dispatch through it.
5. Add focused dispatch tests.

This gives immediate locality around V2/V1 adapter behavior and makes later
model-driven decomposition a caller of a stable dispatch interface rather than
another special path.

Follow-up PR:

1. Wire REPL development/research dispatch through existing Skill routing.
2. Replace the deferred route-hints test with Capability Declaration Route Hint
   coverage.
