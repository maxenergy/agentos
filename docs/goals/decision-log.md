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

## 2026-05-06 G005 Capability Contract Facade

- Added `CapabilityContractValidationResult` and diagnostics to represent declaration and input/output shape validation without moving callers yet.
- The facade wraps existing schema validation messages so current public errors stay unchanged.
- Capability Contract validation remains declaration/shape validation only; runtime authorization remains in `PolicyEngine`.

## 2026-05-06 G011 Runtime Store StorageBackend Seam

- Added `StorageBackend` plus `TsvStorageBackend` as the Runtime Store backend boundary.
- The TSV adapter wraps existing atomic file, append, transaction, manifest, export/import, and migrate helpers without changing on-disk layout.
- Compaction is represented on the backend seam but remains delegated to owning stores until callers move in later goals.

## 2026-05-06 G014 Auth Login Flow Modules

- Extracted Auth Login Flow modes behind `auth_login_flow.*`; provider adapters now select modes but delegate token/session construction to the flow module.
- Login flows use `SecureTokenStore` and `SessionStore` through the existing Credential Store seams and do not select platform token backends.
- `StaticAuthProviderAdapter` remains responsible for descriptors, defaults, endpoint diagnostics, and provider probes.
