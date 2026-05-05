# G011 Runtime Store StorageBackend Seam

Status: ready

## Objective

Introduce a StorageBackend seam for the Runtime Store with a TSV adapter, without adding SQLite.

## Context

Read `CONTEXT.md` terms:

- Runtime Store
- Audit History

Read ADR:

- `docs/ADR-STORAGE-001.md`

## Scope

Allowed files:

- `src/storage/storage_backend.hpp`
- `src/storage/storage_backend.cpp`
- `src/storage/*`
- `src/utils/atomic_file.*` only if the adapter needs a narrow wrapper
- `CMakeLists.txt`
- `tests/storage_tests.cpp`

Out of scope:

- SQLite.
- Changing on-disk TSV formats unless required for a backward-compatible seam.
- Storage command rewrites.

## Requirements

- Define a StorageBackend interface expressing ADR-STORAGE-001 capabilities:
  - atomic replace;
  - append line;
  - transaction prepare/commit/recover;
  - manifest status/verify;
  - export/import;
  - migrate;
  - compact;
  - audit-safe diagnostics.
- Implement a TSV adapter around existing helpers.
- Preserve current runtime file layout and compatibility contract.

## Acceptance

- Existing storage tests pass.
- New tests prove TSV adapter satisfies the seam for core capabilities.
- `CurrentStoragePolicy()` remains consistent with ADR-STORAGE-001.

## Verification

```bash
cmake --build build --target agentos_storage_tests
ctest --test-dir build -R agentos_storage_tests --output-on-failure
git diff --check
```

