# G001 Agent Dispatch Seam

Status: ready

## Objective

Create the Agent Dispatch module and Dispatch Result type without changing runtime behavior.

## Context

Read `CONTEXT.md` terms:

- Agent Dispatch
- Dispatch Result
- Policy Decision
- Subagent Orchestration

## Scope

Allowed files:

- `src/core/orchestration/agent_dispatch.hpp`
- `src/core/orchestration/agent_dispatch.cpp`
- root `CMakeLists.txt` wiring
- focused tests in `tests/subagent_session_tests.cpp` or a new focused test file if the existing target is too crowded

Out of scope:

- Moving AgentLoop onto the new module.
- Moving SubagentManager onto the new module.
- REPL dispatch.
- Capability Contract validation.
- StorageBackend work.
- Auth Login Flow work.

## Requirements

- Define a Dispatch Result type that carries:
  - success/failure state;
  - step candidate;
  - structured agent output or equivalent caller-facing agent payload;
  - error code/message;
  - duration;
  - effective cost.
- Define an Agent Dispatch input type that can represent top-level agent calls and role-scoped subagent calls.
- The seam must be capable of:
  - V2 adapter dispatch;
  - legacy adapter fallback;
  - policy evaluation;
  - lesson-policy hint application;
  - policy audit;
  - cancellation checks;
  - usage-cost vs. legacy estimated-cost selection.
- Do not record final task steps inside Agent Dispatch.

## Acceptance

- The new module compiles and has focused unit coverage using fake adapters and fake policy dependencies.
- Tests prove Dispatch Result contains a step candidate but the module does not own final step recording.
- Tests prove policy-denied dispatch returns a failure Dispatch Result.
- Tests prove V2 usage cost wins over legacy estimated cost.

## Verification

```bash
cmake --build build --target agentos_subagent_session_tests
ctest --test-dir build -R agentos_subagent_session_tests --output-on-failure
git diff --check
```
