# G013 Audit History Recovery Fidelity

Status: done
Depends on: G011

## Objective

Harden Audit History recovery tests and document which reconstructed audit entries are lossy recovery fallbacks.

## Context

Read `CONTEXT.md` terms:

- Runtime Store
- Audit History

## Scope

Allowed files:

- `src/storage/storage_export.*`
- `src/storage/storage_version_store.*`
- storage compaction/recovery helpers
- `tests/storage_tests.cpp`
- `docs/ARCH_ALIGNMENT.md` or storage docs if behavior is documented

Out of scope:

- Making reconstructed Audit History semantically equivalent to original evidence.
- SQLite.

## Requirements

- Treat Audit History as append-only evidence.
- Explicitly test what can be reconstructed from memory and scheduler state.
- Explicitly test or document what cannot be reconstructed.
- Preserve non-task audit events where current behavior promises preservation.

## Acceptance

- Tests distinguish original Audit History from reconstructed fallback.
- Lossy cases are documented in tests or docs.
- Compaction/recovery behavior remains deterministic.

## Verification

```bash
cmake --build build --target agentos_storage_tests
ctest --test-dir build -R agentos_storage_tests --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_storage_tests`
- `ctest --test-dir build-codex-g014 -R agentos_storage_tests --output-on-failure`
- `git diff --check -- tests/storage_tests.cpp docs/goals/G013-audit-history-recovery.md docs/goals/backlog.md docs/goals/decision-log.md`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so a focused diff check was used for this packet.
