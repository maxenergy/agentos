#pragma once

#include "storage/storage_export.hpp"
#include "storage/storage_transaction.hpp"
#include "storage/storage_version_store.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace agentos {

struct StorageBackendDiagnostic {
    std::string code;
    std::string message;
    std::string relative_path;
};

struct StorageManifestStatus {
    bool current = false;
    std::vector<StorageVersionEntry> entries;
    std::vector<StorageBackendDiagnostic> diagnostics;
};

struct StorageVerifyResult {
    bool valid = true;
    int checked_files = 0;
    std::vector<StorageBackendDiagnostic> diagnostics;
};

struct StorageCompactResult {
    bool success = true;
    int compacted_files = 0;
    std::vector<StorageBackendDiagnostic> diagnostics;
};

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual void atomic_replace(const std::filesystem::path& path, std::string_view content) = 0;
    virtual void append_line(const std::filesystem::path& path, std::string_view line) = 0;
    virtual void prepare_transaction(
        std::string_view transaction_id,
        const std::vector<StorageTransactionWrite>& writes) = 0;
    virtual int commit_prepared_transaction(std::string_view transaction_id) = 0;
    virtual StorageTransactionRecoveryResult recover_transactions() = 0;
    virtual StorageManifestStatus manifest_status() const = 0;
    virtual StorageVerifyResult verify_manifest() const = 0;
    virtual StorageExportResult export_state(const std::filesystem::path& destination_root) const = 0;
    virtual StorageImportResult import_state(const std::filesystem::path& source_root) = 0;
    virtual StorageMigrationResult migrate() = 0;
    virtual StorageCompactResult compact(std::string_view target) = 0;
};

class TsvStorageBackend final : public StorageBackend {
public:
    explicit TsvStorageBackend(std::filesystem::path workspace_root);

    void atomic_replace(const std::filesystem::path& path, std::string_view content) override;
    void append_line(const std::filesystem::path& path, std::string_view line) override;
    void prepare_transaction(
        std::string_view transaction_id,
        const std::vector<StorageTransactionWrite>& writes) override;
    int commit_prepared_transaction(std::string_view transaction_id) override;
    StorageTransactionRecoveryResult recover_transactions() override;
    StorageManifestStatus manifest_status() const override;
    StorageVerifyResult verify_manifest() const override;
    StorageExportResult export_state(const std::filesystem::path& destination_root) const override;
    StorageImportResult import_state(const std::filesystem::path& source_root) override;
    StorageMigrationResult migrate() override;
    StorageCompactResult compact(std::string_view target) override;

    const std::filesystem::path& workspace_root() const;
    const std::filesystem::path& runtime_dir() const;

private:
    std::filesystem::path workspace_root_;
    std::filesystem::path runtime_dir_;
    mutable StorageVersionStore storage_version_store_;
};

}  // namespace agentos
