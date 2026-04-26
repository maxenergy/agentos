#include "storage/storage_export.hpp"

#include <chrono>
#include <stdexcept>
#include <cstdlib>
#include <iostream>

namespace agentos {

namespace {

bool StorageDebugEnabled() {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    const auto result = _dupenv_s(&raw_value, &value_size, "AGENTOS_STORAGE_DEBUG");
    if (result != 0 || raw_value == nullptr) {
        return false;
    }

    const std::string value(raw_value, value_size > 0 ? value_size - 1 : 0);
    std::free(raw_value);
    return value == "1";
#else
    if (const char* value = std::getenv("AGENTOS_STORAGE_DEBUG"); value != nullptr) {
        return std::string(value) == "1";
    }
    return false;
#endif
}

void CopyFileIfPresent(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (!std::filesystem::exists(source) || !std::filesystem::is_regular_file(source)) {
        return;
    }

    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path());
    }

    std::error_code error;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        throw std::runtime_error(
            "could not export storage file from " + source.string() + " to " + destination.string() + ": " + error.message());
    }
}

std::filesystem::path NewImportBackupRoot(const std::filesystem::path& workspace_root) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return workspace_root / "runtime" / ".import_backups" / ("import-" + std::to_string(now_ms));
}

}  // namespace

StorageExportResult ExportStorageState(
    const std::filesystem::path& workspace_root,
    const StorageVersionStore& storage_version_store,
    const std::filesystem::path& destination_root) {
    if (destination_root.empty()) {
        throw std::runtime_error("storage export destination is required");
    }

    StorageExportResult result{
        .destination_root = destination_root,
    };

    for (const auto& entry : storage_version_store.list()) {
        const auto source_path = workspace_root / entry.relative_path;
        const auto destination_path = destination_root / entry.relative_path;
        if (StorageDebugEnabled()) {
            std::cerr
                << "storage_export component=" << entry.component
                << " source=" << source_path.string()
                << " destination=" << destination_path.string()
                << '\n';
        }
        if (std::filesystem::exists(source_path) && std::filesystem::is_regular_file(source_path)) {
            CopyFileIfPresent(source_path, destination_path);
            result.exported_files += 1;
        }
    }

    return result;
}

StorageImportResult ImportStorageState(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& source_root) {
    if (source_root.empty()) {
        throw std::runtime_error("storage import source is required");
    }

    StorageVersionStore source_manifest(source_root / "runtime" / "storage_manifest.tsv");
    const auto entries = source_manifest.list();
    if (entries.empty()) {
        throw std::runtime_error("storage import source does not contain a readable runtime/storage_manifest.tsv");
    }

    StorageImportResult result{
        .source_root = source_root,
        .backup_root = NewImportBackupRoot(workspace_root),
    };

    for (const auto& entry : entries) {
        const auto source_path = source_root / entry.relative_path;
        const auto destination_path = workspace_root / entry.relative_path;
        if (std::filesystem::exists(destination_path) && std::filesystem::is_regular_file(destination_path)) {
            CopyFileIfPresent(destination_path, result.backup_root / entry.relative_path);
            result.backed_up_files += 1;
        }
        if (StorageDebugEnabled()) {
            std::cerr
                << "storage_import component=" << entry.component
                << " source=" << source_path.string()
                << " destination=" << destination_path.string()
                << '\n';
        }
        if (std::filesystem::exists(source_path) && std::filesystem::is_regular_file(source_path)) {
            CopyFileIfPresent(source_path, destination_path);
            result.imported_files += 1;
        }
    }

    return result;
}

}  // namespace agentos
