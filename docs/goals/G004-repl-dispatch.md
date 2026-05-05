# G004 REPL Development And Research Dispatch

Status: done
Depends on: G002

## Objective

Wire REPL free-form classification to dispatch `development_request` and `research_request` through normal Skill/Agent routing.

## Context

Read `CONTEXT.md` terms:

- Agent Dispatch
- Capability Declaration
- Route Hint
- Capability Contract

## Scope

Allowed files:

- `src/cli/interactive_commands.cpp`
- `src/cli/interactive_commands.hpp`
- `src/cli/intent_classifier.*`
- `src/skills/builtin/development_skill.*`
- `src/skills/builtin/research_skill.*`
- `tests/cli_integration_tests.cpp`

Out of scope:

- Adding `runtime/skill_routes.tsv` as a separate route-hint source.
- Agent Dispatch seam work.
- `learn_skill` implementation.

## Requirements

- After slash-command dispatch, REPL calls `ClassifyInteractiveRequest`.
- `development_agent` routes to `loop.run({task_type:"development_request", ...})`.
- `research_agent` routes to `loop.run({task_type:"research_request", ...})`.
- Other input still falls through to chat fallback.
- Routed requests are visible in audit and memory like normal tasks.

## Acceptance

- CLI integration coverage proves development and research paths invoke registered skills.
- Chat fallback behavior remains unchanged for non-classified free-form text.
- No `runtime/skill_routes.tsv` loader is introduced unless a separate lifecycle is explicitly justified.

## Verification

```bash
cmake --build build --target agentos_cli_integration_tests
ctest --test-dir build -R agentos_cli_integration_tests --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R agentos_cli_integration_tests --output-on-failure`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so a focused diff check was used for this packet.
