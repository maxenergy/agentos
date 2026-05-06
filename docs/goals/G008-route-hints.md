# G008 Capability Declaration Route Hints

Status: done
Depends on: G005

## Objective

Replace the deferred `runtime/skill_routes.tsv` direction with Route Hint coverage sourced from Capability Declarations.

## Context

Read `CONTEXT.md` terms:

- Capability Declaration
- Route Hint
- Capability Contract

## Scope

Allowed files:

- `src/core/models.hpp` if `SkillManifest` needs a route-hint field
- `src/cli/intent_classifier.*`
- `src/hosts/agents/main_agent.cpp`
- `tests/main_agent_prompt_tests.cpp`
- `tests/cli_integration_tests.cpp`

Out of scope:

- Adding `runtime/skill_routes.tsv`.
- REPL dispatch wiring unless G004 is in progress.
- Learned skill generation unless needed for tests.

## Requirements

- Route Hints are sourced from `SkillManifest`/Capability Declaration metadata.
- Do not create a second truth source for route hints.
- If existing fields are sufficient, document that `description`, `capabilities`, and `input_schema_json` are the Route Hint source.
- Replace or rename stale tests that assume a route TSV loader.

## Acceptance

- Tests prove route-hint data can be consumed from loaded manifests.
- Main agent prompt and/or intent classifier behavior remains deterministic.
- No `runtime/skill_routes.tsv` loader is introduced.

## Verification

```bash
cmake --build build --target agentos_main_agent_prompt_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_main_agent_prompt_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_main_agent_prompt_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_main_agent_prompt_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --check -- tests/main_agent_prompt_tests.cpp README.md docs/goals/G008-route-hints.md docs/goals/backlog.md docs/goals/decision-log.md`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so a focused diff check was used for this packet.
