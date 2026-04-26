#include "storage/storage_policy.hpp"

namespace agentos {

StoragePolicyDecision CurrentStoragePolicy() {
    return {
        .decision_id = "ADR-STORAGE-001",
        .backend = "tsv-mvp",
        .target_backend = "sqlite",
        .migration_status = "deferred",
        .rationale = "current runtime state remains small, repo-local, and already has locking/version/export-import/transaction recovery support",
        .revisit_trigger = "switch when query-heavy workflows, cross-component joins, or multi-process write contention become routine",
        .migration_boundary = "add a StorageBackend interface before introducing sqlite-backed stores",
        .required_interface = "atomic_replace, append_line, transaction_prepare_commit_recover, export_import, migrate, compact, status",
        .compatibility_contract = "preserve runtime/storage_manifest.tsv paths and support one-release TSV import during sqlite migration",
    };
}

}  // namespace agentos
