# G003 Decomposition Planner Through Agent Dispatch

Status: done
Depends on: G002

## Objective

Route the `auto_decompose=true` planner call through Agent Dispatch instead of direct `planner->run_task(...)`.

## Context

Read `CONTEXT.md` terms:

- Agent Dispatch
- Dispatch Result
- Subagent Orchestration

## Scope

Allowed files:

- `src/core/orchestration/subagent_manager.cpp`
- `src/core/orchestration/subagent_manager.hpp`
- `src/core/orchestration/agent_dispatch.*`
- `tests/subagent_session_tests.cpp`
- `tests/cli_integration_tests.cpp`

Out of scope:

- Changing the decomposition output schema.
- Model-driven complex decomposition beyond the current planner flow.
- REPL routing.

## Requirements

- Decomposition planner invocation uses Agent Dispatch.
- Subagent Orchestration still interprets `plan_steps[].action`.
- Dispatch does not interpret decomposition output.
- Cancellation, policy audit, and V2/legacy behavior are consistent with other agent dispatches.
- Decide whether the decomposition step candidate is recorded visibly; document and test the behavior.

## Acceptance

- Existing auto-decompose success and negative-path tests pass.
- New coverage proves decomposition uses the Agent Dispatch seam.
- Invalid planner output remains a Subagent Orchestration concern.

## Verification

```bash
cmake --build build --target agentos_subagent_session_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_subagent_session_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_subagent_session_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_subagent_session_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --cached --check`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so the staged diff check was used for this packet.
