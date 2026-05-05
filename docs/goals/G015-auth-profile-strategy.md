# G015 Auth Profile Strategy

Status: done
Depends on: G014

## Objective

Define Auth Profile selection behavior across API key, OAuth, CLI passthrough, and cloud ADC sessions.

## Context

Read `CONTEXT.md` terms:

- Auth Profile
- Auth Login Flow
- Credential Store

## Scope

Allowed files:

- `src/auth/auth_profile_store.*`
- `src/auth/auth_manager.*`
- `src/auth/session_store.*`
- `src/cli/auth_commands.*`
- `src/cli/auth_interactive.*`
- `tests/auth_tests.cpp`
- `tests/cli_integration_tests.cpp`
- README and auth docs if CLI behavior is clarified

Out of scope:

- Provider endpoint discovery.
- Credential Store backend implementation.

## Requirements

- Avoid the term "account" for credential/session selection.
- Document default profile mapping behavior.
- Ensure profile overrides work uniformly for provider-backed agents and auth commands.
- Clarify behavior when multiple sessions exist for one provider.

## Acceptance

- Tests cover reload/status/default profile behavior.
- CLI help/docs use Auth Profile language.
- Existing `profile=` and `auth_profile=` behavior remains compatible.

## Verification

```bash
cmake --build build --target agentos_auth_tests agentos_agent_provider_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_auth_tests|agentos_agent_provider_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_auth_tests agentos_agent_provider_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_auth_tests|agentos_agent_provider_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --cached --check`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so the staged diff check was used for this packet.
