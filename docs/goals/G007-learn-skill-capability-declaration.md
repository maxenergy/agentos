# G007 Learned Capability Declaration

Status: done
Depends on: G005

## Objective

Complete `learn_skill` so it creates a validated Capability Declaration and reloads it without depending on REPL internals.

## Context

Read `CONTEXT.md` terms:

- Capability Declaration
- Capability Contract
- Route Hint
- Capability Output

## Scope

Allowed files:

- `src/skills/builtin/learn_skill.*`
- `src/cli/skill_reload.*`
- `src/core/registry/skill_registry.*` only if reload seams require it
- `src/main.cpp`
- `CMakeLists.txt`
- `tests/learn_skill_tests.cpp`
- README docs for self-extension

Out of scope:

- REPL dispatch classification.
- Plugin manifests.
- Agent Dispatch.

## Requirements

- `learn_skill` accepts direct CLI spec inputs and optionally `from_url` only if existing project policy permits network use.
- Generated declarations are validated before writing.
- Learned declarations are written under `runtime/cli_specs/learned/`.
- Reload uses a shared helper, not REPL-specific code.
- Name conflicts with protected or already registered builtins are rejected.

## Acceptance

- Tests cover spec write, reload makes the skill findable, name conflict rejection, and malformed args returning `SchemaValidationFailed`.
- CMake target and warning stanza are added.
- README current implementation status mentions self-extension.

## Verification

```bash
cmake --build build --target agentos_learn_skill_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_learn_skill_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_learn_skill_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_learn_skill_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --check -- src/cli/skill_reload.cpp src/skills/builtin/learn_skill.cpp tests/learn_skill_tests.cpp README.md docs/goals/G007-learn-skill-capability-declaration.md docs/goals/backlog.md docs/goals/decision-log.md`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so a focused diff check was used for this packet.
