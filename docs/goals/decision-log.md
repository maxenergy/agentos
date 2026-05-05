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

