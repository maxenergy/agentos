#include "storage/storage_backend.hpp"

#include "utils/atomic_file.hpp"

#include <filesystem>
#include <string>
#include <utility>

namespace agentos {

TsvStorageBackend::TsvStorageBackend(std::filesystem::path workspace_root)
    : workspace_root_(std::move(workspace_root)),
      runtime_dir_(workspace_root_ / "runtime"),
      storage_version_store_(runtime_dir_ / "storage_manifest.tsv") {}

void TsvStorageBackend::atomic_replace(const std::filesystem::path& path, const std::string_view content) {
    WriteFileAtomically(path, std::string(content));
}

void TsvStorageBackend::append_line(const std::filesystem::path& path, const std::string_view line) {
    AppendLineToFile(path, std::string(line));
}

void TsvStorageBackend::prepare_transaction(
    const std::string_view transaction_id,
    const std::vector<StorageTransactionWrite>& writes) {
    PrepareStorageTransaction(runtime_dir_, transaction_id, writes);
}

int TsvStorageBackend::commit_prepared_transaction(const std::string_view transaction_id) {
    return CommitPreparedStorageTransaction(runtime_dir_, transaction_id);
}

StorageTransactionRecoveryResult TsvStorageBackend::recover_transactions() {
    return RecoverStorageTransactions(runtime_dir_);
}

StorageManifestStatus TsvStorageBackend::manifest_status() const {
    const auto entries = storage_version_store_.list();
    return {
        .current = entries == DefaultStorageVersionEntries(),
        .entries = entries,
        .diagnostics = {},
    };
}

StorageVerifyResult TsvStorageBackend::verify_manifest() const {
    StorageVerifyResult result;
    for (const auto& entry : storage_version_store_.list()) {
        result.checked_files += 1;
        const auto path = workspace_root_ / entry.relative_path;
        if (!std::filesystem::exists(path)) {
            result.valid = false;
            result.diagnostics.push_back(StorageBackendDiagnostic{
                .code = "MissingManifestFile",
                .message = "manifested runtime file is missing",
                .relative_path = entry.relative_path,
            });
        } else if (!std::filesystem::is_regular_file(path)) {
            result.valid = false;
            result.diagnostics.push_back(StorageBackendDiagnostic{
                .code = "NonRegularManifestFile",
                .message = "manifested runtime path is not a regular file",
                .relative_path = entry.relative_path,
            });
        }
    }
    return result;
}

StorageExportResult TsvStorageBackend::export_state(const std::filesystem::path& destination_root) const {
    return ExportStorageState(workspace_root_, storage_version_store_, destination_root);
}

StorageImportResult TsvStorageBackend::import_state(const std::filesystem::path& source_root) {
    auto result = ImportStorageState(workspace_root_, source_root);
    storage_version_store_ = StorageVersionStore(runtime_dir_ / "storage_manifest.tsv");
    return result;
}

StorageMigrationResult TsvStorageBackend::migrate() {
    auto result = storage_version_store_.migrate_to_current();
    storage_version_store_ = StorageVersionStore(runtime_dir_ / "storage_manifest.tsv");
    return result;
}

StorageCompactResult TsvStorageBackend::compact(const std::string_view target) {
    StorageCompactResult result;
    result.diagnostics.push_back(StorageBackendDiagnostic{
        .code = "CompactionDelegated",
        .message = "TSV compaction is performed by the owning runtime store",
        .relative_path = std::string(target),
    });
    return result;
}

const std::filesystem::path& TsvStorageBackend::workspace_root() const {
    return workspace_root_;
}

const std::filesystem::path& TsvStorageBackend::runtime_dir() const {
    return runtime_dir_;
}

}  // namespace agentos
