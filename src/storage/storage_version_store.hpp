#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace agentos {

struct StorageVersionEntry {
    std::string component;
    std::string format;
    std::string version;
    std::string relative_path;

    bool operator==(const StorageVersionEntry& other) const = default;
};

struct StorageMigrationResult {
    bool changed = false;
    bool created_manifest = false;
    int updated_entries = 0;
    int migrated_files = 0;
};

class StorageVersionStore {
public:
    explicit StorageVersionStore(std::filesystem::path manifest_path);

    void ensure_current();
    StorageMigrationResult migrate_to_current();
    std::vector<StorageVersionEntry> list() const;
    const std::filesystem::path& manifest_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path manifest_path_;
    std::vector<StorageVersionEntry> entries_;
};

std::vector<StorageVersionEntry> DefaultStorageVersionEntries();

}  // namespace agentos
