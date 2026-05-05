# G002 Move Agent Callers Onto Agent Dispatch

Status: done
Depends on: G001

## Objective

Move `AgentLoop::run_agent_task` and `SubagentManager::run_one` onto the shared Agent Dispatch seam while preserving behavior.

## Context

Read `CONTEXT.md` terms:

- Agent Dispatch
- Dispatch Result
- Subagent Orchestration
- Policy Decision

## Scope

Allowed files:

- `src/core/loop/agent_loop.cpp`
- `src/core/loop/agent_loop.hpp`
- `src/core/orchestration/subagent_manager.cpp`
- `src/core/orchestration/subagent_manager.hpp`
- `src/core/orchestration/agent_dispatch.*`
- `tests/subagent_session_tests.cpp`
- `tests/cli_integration_tests.cpp` only for existing behavior coverage

Out of scope:

- Decomposition planner dispatch.
- REPL development/research routing.
- Plugin and storage work.

## Requirements

- `AgentLoop` uses Agent Dispatch for selected agent targets.
- `SubagentManager::run_one` uses Agent Dispatch for individual subagent calls.
- AgentLoop still owns top-level TaskRunResult finalization.
- Subagent Orchestration still owns when step candidates are recorded, preserving parallel ordering behavior.
- Policy audit remains inside Agent Dispatch.
- Route audit and task start/end remain with callers.

## Acceptance

- Existing V2 preference tests remain green.
- Existing cancellation tests remain green.
- Existing auth profile forwarding tests remain green.
- Duplicated projection assertions are reduced or moved to Agent Dispatch tests.

## Verification

```bash
cmake --build build --target agentos_subagent_session_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_subagent_session_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_agent_dispatch_tests agentos_subagent_session_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_agent_dispatch_tests|agentos_subagent_session_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --check -- src/core/loop/agent_loop.cpp src/core/orchestration/subagent_manager.cpp src/core/orchestration/agent_dispatch.cpp src/core/orchestration/agent_dispatch.hpp`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so the focused touched-file check was used for this packet.
