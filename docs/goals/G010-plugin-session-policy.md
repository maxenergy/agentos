# G010 Plugin Session Process-Pool Policy

Status: blocked
Depends on: G009

## Objective

Clarify Plugin Session process-pool policy and session administration scope without introducing a generic resident capability session abstraction.

## Context

Read `CONTEXT.md` terms:

- Plugin Session
- Capability Output

## Scope

Allowed files:

- `src/hosts/plugin/plugin_host.*`
- `src/hosts/plugin/plugin_persistent_session.hpp`
- `src/cli/plugins_commands.*`
- `tests/cli_plugin_tests.cpp`
- `docs/ARCH_ALIGNMENT.md` or README if CLI semantics change

Out of scope:

- Agent workspace sessions.
- Generic resident capability sessions.
- New protocol support.

## Requirements

- Keep Plugin Session lifecycle separate from agent workspace sessions.
- Preserve explicit process-scope diagnostics.
- Improve process-pool policy reporting where behavior is currently implicit.
- Make unsupported future scopes fail clearly.

## Acceptance

- Session admin remains scriptable.
- Tests cover process-scope no-op, match, prune, restart, close, and unsupported scope behavior.
- Documentation states the current session admin boundary.

## Verification

```bash
cmake --build build --target agentos_cli_plugin_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_cli_plugin_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

