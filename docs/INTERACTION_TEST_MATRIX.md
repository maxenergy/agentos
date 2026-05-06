# AgentOS Interaction Test Matrix

This matrix defines fast, scenario-driven tests for AgentOS interaction behavior. The default rule is: use deterministic fixtures for CI and reserve real network/model runs for manual smoke probes.

## Test Tiers

### Tier 0: Fast Unit And Module Tests

Run on every change:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected duration: under 10 seconds on a warmed Linux build.

Purpose:

- Validate routing, policy, storage, auth, workflow, cancellation, and orchestration modules without real external agents.
- Keep failures deterministic.
- Avoid real internet, OAuth, or LLM calls.

### Tier 1: Fast Interactive CLI Fixtures

Run when changing REPL, routing, job state, agent skills, or task classification:

```bash
cmake --build build --target agentos_cli_integration_tests
ctest --test-dir build -R agentos_cli_integration_tests --output-on-failure
```

These tests should use workspace-local fake binaries such as fake `codex`, fake `curl`, fake `jq`, and empty `PATH` directories. They should never depend on a configured real Codex/Claude/Gemini/OpenAI account.

### Tier 2: Manual Smoke Probes

Run only before releases or after large orchestration changes:

```bash
build/agentos interactive
```

Use real agents and network. These probes can be slow and flaky because they depend on model providers, network, and rate limits.

## Core Scenario Matrix

| ID | User Scenario | Input | Expected Route | Execution Mode | What It Proves | Fast Test Hook |
| --- | --- | --- | --- | --- | --- | --- |
| I-001 | Simple Chinese chat | `你好` | `chat_agent` | `sync` | REPL preserves UTF-8 and routes non-command text | PTY fixture in `agentos_cli_integration_tests` |
| I-002 | Runtime skill list | `你现在有什么技能？` or `skills` | `local_intent` / command | `sync` | Runtime skills and repo agent skills are presented separately | CLI fixture with `.agents/skills/sample/SKILL.md` |
| I-003 | Development request | `please build a small command line tool` | `development_agent -> development_request` | `async_job` | REPL enqueues development job and returns prompt | Empty `PATH` and fake `codex` fixture |
| I-004 | Research request | `please research current provider integration details` | `research_agent -> research_request` | `async_job` | Research no longer blocks the REPL | Empty `PATH` fixture |
| I-005 | Mixed research plus artifact | `研究这个仓库如何加入技能库: <url>` | Prefer `research_agent` unless user explicitly asks to edit/install | `async_job` | Prevents research-only prompts from becoming premature development tasks | Add classifier regression |
| I-006 | Explicit install/edit | `把这个技能库安装到 .agents/skills` | `development_agent` | `async_job` | Explicit workspace mutation routes to development | Classifier regression |
| I-007 | Running jobs | `jobs` after enqueue | command | `sync` | Job list shows kind, state, task dir, status, wait policy | Fake slow `codex` fixture |
| I-008 | Exit with active job | `exit` while job running | command | `sync` | REPL warns and returns prompt instead of blocking | Fake slow `codex` fixture |
| I-009 | Explicit wait on exit | `exit --wait` | command | `sync` | User can deliberately wait for job completion | Fake slow `codex` fixture |

## Orchestration And Delegation Scenarios

| ID | User Scenario | Command Or Stimulus | Expected Behavior | What It Proves | Fast Test Hook |
| --- | --- | --- | --- | --- | --- |
| O-001 | Agent dispatch to a healthy V2 agent | `run analysis target=<fixture>` | Agent route emits route/step/task_end audit records | Agent Dispatch seam works | `agentos_agent_dispatch_tests` |
| O-002 | Agent unavailable | Development/research with empty `PATH` | `AgentUnavailable`, audit and memory still record task | Failure is visible and recoverable | `agentos_cli_integration_tests` |
| O-003 | Sequential subagents | `agentos subagents run mode=sequential ...` with fixture agents | Steps execute in order and aggregate result is stable | Subagent orchestration sequence | `agentos_subagent_session_tests` |
| O-004 | Parallel subagents | `agentos subagents run mode=parallel ...` with fixture agents | Independent agents run and results aggregate | Parallel orchestration | `agentos_subagent_session_tests` |
| O-005 | Cancellation before route | Tripped cancellation token | `Cancelled` before route selection | Main loop cancellation boundary | `agentos_cancellation_tests` |
| O-006 | Cancellation during dispatch | Fake long-running V2 agent observes token | `Cancelled` step/result | Agent dispatch cancellation propagation | `agentos_cancellation_tests` |

## Acceptance And Artifact Scenarios

| ID | User Scenario | Fixture Behavior | Expected Acceptance | What It Proves | Fast Test Hook |
| --- | --- | --- | --- | --- | --- |
| A-001 | Development succeeds with deliverables | Fake `codex` writes deliverables manifest and target file | `accepted=true`, one attempt | Acceptance can verify artifacts | Add focused development skill fixture test |
| A-002 | Agent exits 0 but writes no artifact | Fake `codex` returns success text only | `AcceptanceFailed`, repair attempt scheduled | Prose-only output is not accepted | Add focused development skill fixture test |
| A-003 | Environment blocker | Fake `codex` reports missing dependency | `AcceptanceBlocked`, no endless repair | Blockers stop repair loop | Add focused development skill fixture test |
| A-004 | Repair succeeds | Attempt 1 missing manifest; attempt 2 writes manifest | final `accepted=true`, attempt_count=2 | Repair loop uses prior acceptance feedback | Add focused development skill fixture test |
| A-005 | Research produces sources | Fake network agent emits text with source URLs | `accepted=true` for research skill result | Research skill captures output/events/status | Add focused research skill fixture test |

## Workflow And Memory Scenarios

| ID | User Scenario | Stimulus | Expected Behavior | What It Proves | Fast Test Hook |
| --- | --- | --- | --- | --- | --- |
| W-001 | Built-in workflow | `run workflow_run workflow=write_patch_read ...` | File write, patch, read steps complete | Skill composition works | Existing workflow tests plus CLI probe |
| W-002 | Stored workflow disabled | Disabled workflow definition | `WorkflowDisabled` | Stored workflow policy enforced | `agentos_workflow_router_tests` |
| W-003 | Recursive workflow rejection | Stored workflow includes `workflow_run` | Validation rejects recursion | Prevents runaway composition | Workflow skill test |
| W-004 | Workflow promotion threshold | Synthetic task history | Majority signature promotes only at threshold | Memory-driven routing remains stable | `agentos_workflow_router_tests` |
| W-005 | Audit recovery | Corrupt/missing audit with memory task log | Recovered task_start/route/step/task_end | Runtime Store recovery works | `agentos_storage_tests` |

## Skill Library Scenarios

| ID | User Scenario | Input | Expected Behavior | What It Proves | Fast Test Hook |
| --- | --- | --- | --- | --- | --- |
| S-001 | Runtime skill registration | `run learn_skill name=echo_test binary=echo ...` | New CLI skill appears in registry | `learn_skill` Capability Declaration path | `agentos_learn_skill_tests` |
| S-002 | URL skill collection research | `研究这个项目如何加入技能库: <repo>` | Research first; no workspace mutation unless explicitly requested | Avoids premature development route | Classifier regression |
| S-003 | Repo agent skill install | `tools/install_maxenergy_skills.sh --repo-scope` | `.agents/skills/<name>/SKILL.md` exists | Agent skill library install path | Shell syntax plus filesystem check |
| S-004 | Agent skills visible | `skills` command in REPL | Shows repo-level agent skills separately | User sees installed agent skills | `agentos_cli_integration_tests` |
| S-005 | Runtime vs agent skill explanation | `你现在有什么技能？` | Explains `run` applies to runtime skills only | Prevents semantic confusion | CLI integration output assertion |

## Policy, Trust, And Security Scenarios

| ID | User Scenario | Stimulus | Expected Behavior | What It Proves | Fast Test Hook |
| --- | --- | --- | --- | --- | --- |
| P-001 | File read allowed | Low-risk file read skill | Success | Baseline permission path | `agentos_file_skill_policy_tests` |
| P-002 | File write denied without grant | Medium-risk write skill under restricted policy | `PolicyDenied` | Policy gate prevents writes | `agentos_policy_trust_tests` |
| P-003 | Protected skill name | `learn_skill name=development_request ...` | Schema validation rejects | Runtime skill names protected | `agentos_learn_skill_tests` |
| P-004 | Plugin output schema invalid | Plugin returns malformed output | Normalized schema failure | Plugin boundary is typed | `agentos_cli_plugin_tests` |
| P-005 | Credential store fallback | Missing Secret Service | Env-ref fallback works | Linux dependency absence is safe | `agentos_auth_tests` |

## Recommended New Fast Tests

Add these before doing more manual REPL probing:

1. `intent_classifier_tests.cpp`
   - Research-only URL skill-library request routes to `research_agent`.
   - Explicit install/edit request routes to `development_agent`.
   - Mixed research plus artifact wording does not automatically imply workspace mutation.

2. `development_skill_fixture_tests.cpp`
   - Fake `codex` success with valid `deliverables.json`.
   - Fake `codex` success without deliverables triggers acceptance failure.
   - Fake first failure plus second success verifies repair loop.

3. `research_skill_fixture_tests.cpp`
   - Fake network-capable agent emits events and a source URL.
   - Verify `events.jsonl`, `status.json`, audit, and memory records.

4. `agent_event_runtime_store_tests.cpp`
   - Given nested attempts with multiple `status.json` files, latest status is selected.
   - Malformed status does not crash and still returns the status path.

5. `interactive_job_runtime_tests.cpp`
   - Enqueue development/research jobs using fake agents.
   - `jobs` shows kind/status/policy.
   - `exit` vs `exit --wait` behavior remains stable.

## Manual Smoke Script

Use this only after the fast suite passes:

```text
agentos> 你好
agentos> 你现在有什么技能？
agentos> 帮我搜索今天的 AI 新闻
agentos> jobs
agentos> 帮我写一个 3 页 PPT 大纲并保存为文件
agentos> jobs
agentos> exit
agentos> exit --wait
```

Pass criteria:

- The REPL prompt returns immediately after research/development enqueue.
- `jobs` shows stable job IDs, kind, status file, and wait policy.
- Research tasks do not mutate workspace unless asked.
- Development tasks produce declared artifacts and acceptance reports.
- `exit` does not block when jobs are active; `exit --wait` waits deliberately.

## CI Recommendation

Keep default CI on Tier 0 and Tier 1:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Do not put Tier 2 real provider probes in CI. They should be manual release checks or nightly diagnostics with credentials, larger timeouts, and explicit network allowance.
