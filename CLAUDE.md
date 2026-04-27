# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

C++20 + CMake (≥ 3.20), Ninja generator. The build configures `agentos_core` (static lib), the `agentos` CLI executable, and 12 test executables registered with CTest.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows without MSVC env loaded, prefix with the VS dev cmd shim (see `README.md` lines 99-108). MSVC is built with `/W4 /permissive- /EHsc`; GCC/Clang with `-Wall -Wextra -Wpedantic`. Treat new warnings as bugs.

Single test target:

```bash
ctest --test-dir build --output-on-failure -R agentos_workflow_router_tests
# or run the binary directly for closer iteration
build/agentos_workflow_router_tests
```

The test binaries are: `agentos_cli_integration_tests`, `agentos_storage_tests`, `agentos_auth_tests`, `agentos_agent_provider_tests`, `agentos_policy_trust_tests`, `agentos_scheduler_tests`, `agentos_workflow_router_tests`, `agentos_subagent_session_tests`, `agentos_cancellation_tests`, `agentos_cli_plugin_tests`, `agentos_spec_parsing_tests`, `agentos_file_skill_policy_tests`. Adding a new test means adding both the source under `tests/` and a matching `add_executable` + `add_test` block in `CMakeLists.txt` (and the MSVC/GCC warning-flag stanza at the bottom).

CI (`.github/workflows/ci.yml`) runs the same configure/build/test on Ubuntu and Windows for every push and PR.

## CLI surface

All runtime exploration goes through `build/agentos[.exe]`. Top-level subcommands dispatched in `src/main.cpp`:

`demo`, `run <skill> k=v ...`, `cli-demo`, `agents`, `cli-specs`, `plugins`, `auth`, `memory`, `storage`, `schedule`, `subagents`, `trust`.

`run` arguments use `key=value`; reserved keys (`objective`, `target`, `idempotency_key`, `user`, `remote`, `origin_identity`, `origin_device`, `allow_network`, `allow_high_risk`, `approval_id`, `permission_grants`, `timeout_ms`, `budget_limit`) are pulled into `TaskRequest`; everything else lands in `TaskRequest.inputs`. See `BuildTaskFromArgs` in `src/main.cpp` for the canonical mapping. The `README.md` "常用命令" section has worked examples for every subcommand.

## Architecture

### Composition root

`src/main.cpp::Runtime` is the single composition root: it owns every store, registry, host, and policy/audit/memory component, wires them into the `AgentLoop`, and hands the loop a `TaskRequest`. There is no DI framework — adding a subsystem means: own its state on `Runtime`, pass references to its collaborators in the constructor init list, and register skills/agents in the bootstrap path.

### Execution path

A task flows: `AgentLoop` → `Router` (SkillRouter / WorkflowRouter / AgentRouter) → `PolicyEngine` → executor (`SkillRegistry` / `AgentRegistry` / workflow runner) → result normalization → `MemoryManager` (TaskLog/StepLog/stats/lessons/workflow candidates) → `AuditLogger`. The router is a thin shell over three sub-routers in `src/core/router/`; routing decisions are influenced by `MemoryManager` stats and `LessonStore` hints (repeated failures suppress workflows, downrank agents, attach denial hints).

### Capability surfaces

Four ways a capability can plug into the kernel — each has its own adapter interface and host:

- **Builtin Skill** (`src/skills/builtin/`): in-process C++. Used for `file_read`, `file_write`, `file_patch`, `http_fetch`, `workflow_run`. Direct side-effect skills require `idempotency_key`; the loop dedupes via `runtime/execution_cache.tsv`.
- **CLI Skill** (`src/hosts/cli/`): external binaries declared in `runtime/cli_specs/*.tsv` and registered at startup via `CliSpecLoader`. `CliHost` enforces cwd, timeout, env allowlist, output caps, secret redaction, and resource limits (Windows Job Object / POSIX `setrlimit`).
- **Plugin Skill** (`src/hosts/plugin/`): external processes declared in `runtime/plugin_specs/*.tsv` or `*.json`. Three protocols — `plugin.v1`, `stdio-json-v0`, `json-rpc-v0` — picked by manifest. Manifests must declare `process.spawn`. `json-rpc-v0` validates the response and projects `result` against `output_schema_json`. `lifecycle_mode=persistent` (json-rpc only) keeps a long-lived stdio session. `sandbox_mode=workspace` (default) rejects path args resolving outside the workspace.
- **Subagent** (`src/hosts/agents/` + `src/core/orchestration/subagent_manager.cpp`): `IAgentAdapter` wraps an external model/CLI as a callable agent. Current adapters: `local_planner` (offline), `codex_cli`, `gemini`, `anthropic`, `qwen`. `SubagentManager` runs sequential/parallel orchestration with role assignment, per-agent objectives, optional `auto_decompose=true` planning step, concurrency/cost limits, and `WorkspaceSession` lifecycle. It reuses Policy/Audit/Memory rather than re-implementing them.

When adding a new capability, choose the surface deliberately — do **not** add provider-specific logic to `core/` (see `docs/CODING_GUIDE.md` §2.3). Adapters isolate provider differences.

### Auth / Trust / Policy

Three orthogonal concerns, all consulted before execution:

- **Auth** (`src/auth/`): `AuthManager` + per-provider `IAuthProviderAdapter` (OpenAI/Codex, Anthropic, Gemini, Qwen). `SessionStore` (metadata, TSV) and `SecureTokenStore` (Windows Credential Manager; other platforms are env-ref-only dev fallback) are kept separate. `CredentialBroker` is the single read path for tokens. Importable login states: Codex CLI, Claude CLI, Gemini CLI OAuth, Google ADC. PKCE OAuth implemented in `src/auth/oauth_pkce.cpp`. API-key profiles store env-var refs, never plaintext.
- **Trust** (`src/trust/`): identities, allowlist (paired devices with `last_seen` lifecycle), pairing invites, `TrustPolicy`. Remote-triggered tasks are denied by default — they require a paired identity/device with `task.submit`.
- **Policy** (`src/core/policy/`): `PolicyEngine` consults `RoleCatalog`, `ApprovalStore`, and the trust policy. High-risk skills require `approval_id` referencing an approved request in `runtime/trust/approvals.tsv`. Permission grants on `TaskRequest` widen capability per-call.

Sensitive values must flow through `SecretRedactor` (`src/utils/secret_redaction.cpp`) before hitting logs. Errors use named codes (`PolicyDenied`, `SkillNotFound`, `Timeout`, `ExternalProcessFailed`, …), not strings.

### Memory & evolution

`MemoryManager` writes TaskLog/StepLog/skill-stats/agent-stats. `LessonStore` derives hints from repeated failures. `WorkflowStore` promotes recurring step sequences from candidates into named workflows; routing then prefers them when their `required_inputs` / `input_equals` / numeric range / regex / `input_any` / `input_expr` conditions match. `agentos memory promote-workflow`, `validate-workflows`, and `explain-workflow` manage and debug this.

### Persistence layout

Everything stateful lives under `runtime/` (gitignored). All formats are TSV, written via `src/utils/atomic_file.cpp` for atomic replace. Don't introduce ad-hoc file IO — go through the existing stores. The full layout is enumerated in `README.md` lines 200-219.

`storage_version_store.ensure_current()` runs on Runtime construction; schema migrations live in `src/storage/`.

### Scheduler

`Scheduler` persists one-shot/interval/cron tasks (`runtime/scheduler/tasks.tsv`) plus run history. Cron supports five-field syntax, `@hourly`/`@daily`/`@weekly`/`@monthly`/`@yearly` aliases, and DOM/DOW OR semantics. Scheduler-driven runs reuse `AgentLoop` and auto-generate idempotency keys per run.

## Conventions worth knowing

- **Plan tracking**: `plan.md` is the canonical TODO/completion ledger; update it when finishing a capability. `completion_review.md` is a current-state audit.
- **Docs are normative**: `docs/ARCHITECTURE.md`, `docs/AUTH_DESIGN.md`, `docs/SKILL_SYSTEM.md`, `docs/AGENT_SYSTEM.md`, `docs/MEMORY_EVOLUTION.md`, `docs/CLI_INTEGRATION.md`, `docs/CODING_GUIDE.md`, `docs/ROADMAP.md`. `docs/ARCH_ALIGNMENT.md` tracks where the implementation has drifted from the original plan.
- **Tests prefer fixtures over real externals**: integration tests for CLI behavior go through `agentos_cli_integration_tests`, which spawns the built `agentos` binary (path injected via `AGENTOS_CLI_TEST_EXE`). For provider adapters, prefer fixture/mock adapters in unit tests; reserve real network calls for opt-in integration.
- **No SQLite yet**: despite docs/CODING_GUIDE.md mentioning it as a recommendation, current persistence is TSV-only. Don't add a SQLite dependency without an explicit decision.
