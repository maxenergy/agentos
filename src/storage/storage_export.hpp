#pragma once

#include "storage/storage_version_store.hpp"

#include <filesystem>

namespace agentos {

struct StorageExportResult {
    int exported_files = 0;
    std::filesystem::path destination_root;
};

struct StorageImportResult {
    int imported_files = 0;
    int backed_up_files = 0;
    std::filesystem::path source_root;
    std::filesystem::path backup_root;
};

StorageExportResult ExportStorageState(
    const std::filesystem::path& workspace_root,
    const StorageVersionStore& storage_version_store,
    const std::filesystem::path& destination_root);

StorageImportResult ImportStorageState(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& source_root);

}  // namespace agentos
