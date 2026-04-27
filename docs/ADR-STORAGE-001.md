# ADR-STORAGE-001: MVP Storage Backend Boundary

Status: Accepted

Date: 2026-04-26

Owners: AgentOS core/runtime/storage

Related:
- `plan.md`
- `docs/ARCHITECTURE.md`
- `docs/ARCH_ALIGNMENT.md`
- `docs/ROADMAP.md`
- `src/storage/storage_policy.cpp`
- `tests/storage_tests.cpp`

## Context

AgentOS currently stores local runtime state in repo-local TSV files under
`runtime/`. The implementation is intentionally simple and inspectable, and it
now includes enough reliability support for the MVP:

- full-file TSV stores write through temporary files plus atomic replacement;
- append-only logs use locked appends and append-intent recovery;
- multi-file maintenance flows can use prepare/commit/recover markers;
- `runtime/storage_manifest.tsv` records managed files and versions;
- `agentos storage status` reports the active policy and file diagnostics;
- `agentos storage verify`, `export`, `import`, `backups`, `restore-backup`,
  `migrate`, `recover`, and `compact` cover operational workflows.

Earlier planning left the door open for SQLite. That migration should not start
until there is a concrete need, because adding a second backend before the
storage boundary is explicit would make the runtime harder to audit and would
risk breaking the TSV import/export path that is useful for development and
support.

## Decision

TSV remains the MVP storage backend. SQLite is the deferred target backend, not
an active dependency.

Before adding SQLite-backed stores, AgentOS must introduce an explicit
`StorageBackend` interface. The boundary must preserve current command behavior
and provide a one-release TSV import compatibility path during migration.

The current policy is surfaced in code through `CurrentStoragePolicy()` and in
operator output through `agentos storage status`:

- `decision_id=ADR-STORAGE-001`
- `backend=tsv-mvp`
- `target_backend=sqlite`
- `migration_status=deferred`
- `migration_boundary=add a StorageBackend interface before introducing sqlite-backed stores`
- `compatibility_contract=preserve runtime/storage_manifest.tsv paths and support one-release TSV import during sqlite migration`

## Required Backend Capabilities

A future backend must cover the capabilities that current TSV stores already
expose to AgentOS workflows:

- atomic replacement for full-file stores;
- append-line writes with crash recovery;
- prepare/commit/recover support for multi-file or multi-record maintenance;
- manifest-style status and verification;
- export/import support for backups and workspace transfer;
- migration and schema normalization hooks;
- compaction and recovery entry points;
- audit-safe diagnostics that do not expose secrets.

The current concrete capability string is:

`atomic_replace, append_line, transaction_prepare_commit_recover, export_import, migrate, compact, status`

## Revisit Triggers

Revisit the SQLite migration when at least one of these becomes routine:

- query-heavy workflows that are awkward or expensive over TSV;
- cross-component joins that are needed for user-facing features;
- sustained multi-process write contention beyond the current lock model;
- data volume where TSV compaction and linear scans become a measured problem;
- transaction semantics that exceed the current prepare/commit/recover helper.

## Consequences

Keeping TSV for the MVP avoids a new embedded database dependency and preserves
human-readable runtime state. It also keeps export/import, support, and test
fixtures straightforward.

The tradeoff is that AgentOS keeps limited ad hoc querying, limited
cross-process concurrency, and no relational integrity until the
`StorageBackend` boundary is implemented. New storage features should avoid
coupling directly to SQLite assumptions until this ADR is superseded.

## Verification

The decision is covered by:

- `tests/storage_tests.cpp::TestStoragePolicyDecisionDefaultsToTsvMvp`
- `tests/cli_integration_tests.cpp` storage status coverage
- `agentos storage status` policy output
- `agentos storage verify` manifest diagnostics

