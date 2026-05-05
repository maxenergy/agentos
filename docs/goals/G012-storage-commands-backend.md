# G012 Storage Commands Through StorageBackend

Status: done
Depends on: G011

## Objective

Route storage status, verify, export, import, migrate, recover, and compact through the StorageBackend seam.

## Context

Read `CONTEXT.md` terms:

- Runtime Store
- Audit History

Read ADR:

- `docs/ADR-STORAGE-001.md`

## Scope

Allowed files:

- `src/cli/storage_commands.*`
- `src/storage/*`
- `tests/storage_tests.cpp`
- `tests/cli_integration_tests.cpp`

Out of scope:

- SQLite.
- New storage commands unless required to preserve existing behavior.
- Auth/session store refactors.

## Requirements

- Storage CLI behavior remains compatible.
- Manifest-managed file diagnostics come from the backend seam.
- Export/import/verify/migrate/compact callers stop reaching around the seam for backend-specific details where practical.
- Error messages stay scriptable.

## Acceptance

- Existing storage CLI integration tests pass.
- New tests prove command behavior uses the backend seam where observable.
- Storage policy output still reports ADR-STORAGE-001.

## Verification

```bash
cmake --build build --target agentos_storage_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_storage_tests|agentos_cli_integration_tests" --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_storage_tests agentos_cli_integration_tests`
- `ctest --test-dir build-codex-g014 -R "agentos_storage_tests|agentos_cli_integration_tests" --output-on-failure`
- `git diff --check -- src/cli/storage_commands.cpp src/storage/storage_backend.cpp tests/cli_integration_tests.cpp`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so the focused touched-file check was used for this packet.
