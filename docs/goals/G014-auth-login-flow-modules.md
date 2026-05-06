# G014 Auth Login Flow Modules

Status: done

## Objective

Extract Auth Login Flow mode implementations from `StaticAuthProviderAdapter` while preserving behavior.

## Context

Read `CONTEXT.md` terms:

- Auth Login Flow
- Auth Profile
- Credential Store

## Scope

Allowed files:

- `src/auth/provider_adapters.*`
- new `src/auth/auth_login_flow.*` or mode-specific auth files
- `src/auth/oauth_pkce.*` only for narrow orchestration extraction
- `tests/auth_tests.cpp`
- `tests/agent_provider_tests.cpp`
- `tests/cli_integration_tests.cpp`

Out of scope:

- Credential Store backend selection.
- New provider endpoints.
- Auth Profile strategy changes beyond preserving existing profile behavior.

## Requirements

- Extract mode-specific logic for:
  - API key env-ref login;
  - CLI session passthrough;
  - browser OAuth PKCE;
  - cloud ADC;
  - refresh.
- Provider adapters remain focused on descriptors, defaults, and probes.
- Login modes call Credential Store through the existing seam; they do not select platform backends.
- Preserve endpoint status diagnostics.

## Acceptance

- Existing auth tests pass.
- New tests exercise mode modules through fixture providers/stores.
- Provider-specific branches in `StaticAuthProviderAdapter::login` are reduced.

## Verification

```bash
cmake --build build --target agentos_auth_tests agentos_agent_provider_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_auth_tests|agentos_agent_provider_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake -S . -B build-codex-g014 -G Ninja`
- `cmake --build build-codex-g014 --target agentos_auth_tests agentos_agent_provider_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_auth_tests|agentos_agent_provider_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --check -- tests/auth_tests.cpp`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so the focused touched-file check was used for this packet.
