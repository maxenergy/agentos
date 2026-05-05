# G016 Provider OAuth Defaults Watch

Status: blocked
Depends on: external provider

## Objective

Track Anthropic public PKCE availability and promote Provider OAuth Defaults only when stable customer endpoints exist.

## Context

Read `CONTEXT.md` terms:

- Auth Login Flow
- Auth Profile

## Scope

Allowed files when endpoints become stable:

- `src/auth/oauth_pkce.*`
- `src/auth/provider_adapters.*`
- `tests/auth_tests.cpp`
- `tests/cli_integration_tests.cpp`
- `docs/AUTH_PRD.md`
- `docs/AUTH_DESIGN.md`
- README

Out of scope:

- Guessing undocumented endpoints.
- Marking a provider `builtin` without stable public customer flow.
- Credential Store changes.

## Requirements

- Keep Anthropic PKCE as `deferred` while stable endpoints are unavailable.
- When endpoints exist, promote defaults from stub/deferred to builtin/available.
- Add interoperability or deterministic fixture coverage for endpoint status and login orchestration.
- Keep workspace override behavior.

## Acceptance

- `auth oauth-defaults` and `auth oauth-config-validate --all` report machine-readable endpoint status.
- Docs distinguish builtin defaults from workspace overrides.
- Tests cover deferred and available paths.

## Verification

```bash
cmake --build build --target agentos_auth_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_auth_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

