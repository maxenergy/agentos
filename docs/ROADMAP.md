# AgentOS Roadmap

Last synced: 2026-04-23

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
- `PolicyEngine`
- `PermissionModel`
- `AuditLogger`
- CMake project and smoke test target

Remaining:

- Router decomposition into SkillRouter / AgentRouter / WorkflowRouter
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

Remaining:

- Schema validation
- Stored workflow execution beyond built-in workflow names
- Idempotency enforcement for side-effecting skills

---

## Phase 3: CLI Host

Status: 🚧 Partial

Delivered:

- `CliHost`
- `CliSkillInvoker`
- `rg_search`
- `git_status`
- `git_diff`
- `curl_fetch`
- cwd boundary checks
- timeout
- stdout/stderr capture
- output limit
- env allowlist

Remaining:

- External CLI spec loader
- `jq_transform`
- command redaction for sensitive arguments
- stronger OS resource limits

---

## Phase 4: Secondary Agent Adapter

Status: 🚧 Partial

Delivered:

- `IAgentAdapter`
- `mock_planner`
- `codex_cli`
- Agent health listing
- Router selection by health and basic historical score

Remaining:

- Claude / Gemini / Qwen agent adapters
- WorkspaceSession
- session-aware agent execution
- richer result normalization

---

## Phase 5: Auth Subsystem

Status: 🚧 Partial

Delivered:

- `AuthManager`
- Provider adapters
- `SessionStore`
- `SecureTokenStore` env-ref MVP
- `CredentialBroker`
- API-key profile support
- Codex / Claude CLI session probes
- `agentos auth ...` command group

Remaining:

- OAuth PKCE flow
- refresh flow
- system credential store integration
- cloud credential modes
- workspace default profile mapping

---

## Phase 6: Memory And Workflow Learning

Status: 🚧 Partial

Delivered:

- Task log
- Step log
- Skill stats
- Agent stats
- Workflow candidate generation
- Workflow scoring
- `agentos memory ...` command group

Remaining:

- LessonStore
- durable WorkflowStore
- workflow promotion
- auto workflow selection by Router
- SQLite or versioned storage migration

---

## Phase 7: Identity, Trust, And Policy

Status: ✅ Implemented MVP

Delivered:

- `IdentityManager`
- `PairingManager`
- `AllowlistStore`
- `TrustPolicy`
- remote trigger identity/device fields
- pairing-required remote task policy
- `agentos trust ...` command group

Remaining:

- pairing handshake UX
- role/user-level authorization
- device lifecycle management
- audit events for trust mutations

---

## Phase 8: Scheduler

Status: 🚧 Partial

Delivered:

- `Scheduler`
- persisted `ScheduledTask`
- one-shot tasks
- interval tasks
- manual `schedule run-due`

Remaining:

- background `schedule tick` / daemon mode
- cron grammar
- retry/backoff
- missed-run semantics
- scheduler-specific execution metadata

---

## Phase 9: Subagent Orchestration

Status: 🚧 Partial

Delivered:

- `SubagentManager`
- explicit agent list orchestration
- sequential mode
- parallel mode
- shared Policy / Audit / Memory path
- `agentos subagents run ...`

Remaining:

- automatic subagent candidate selection
- task decomposition
- role assignment
- WorkspaceSession
- cost and concurrency limits

---

## Phase 10: Plugin And Ecosystem

Status: ❌ Not implemented

Remaining:

- Plugin Host
- plugin manifest
- stdio / JSON-RPC protocol
- sandboxing model
- plugin tests

---

## Current Priority

Follow the "Immediate Next TODO" section in [`../plan.md`](../plan.md).
