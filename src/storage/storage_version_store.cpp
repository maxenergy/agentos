#include "storage/storage_version_store.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

std::string EncodeField(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r') {
            constexpr char hex[] = "0123456789ABCDEF";
            output << '%' << hex[(ch >> 4) & 0x0F] << hex[ch & 0x0F];
        } else {
            output << static_cast<char>(ch);
        }
    }
    return output.str();
}

int HexValue(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

std::string DecodeField(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = HexValue(value[index + 1]);
            const auto low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(value[index]);
    }
    return output;
}

std::vector<std::string> SplitLine(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto delimiter = line.find(kDelimiter, start);
        if (delimiter == std::string::npos) {
            parts.push_back(DecodeField(line.substr(start)));
            break;
        }
        parts.push_back(DecodeField(line.substr(start, delimiter - start)));
        start = delimiter + 1;
    }
    return parts;
}

std::filesystem::path WorkspaceRootFromManifest(const std::filesystem::path& manifest_path) {
    const auto runtime_dir = manifest_path.parent_path();
    if (runtime_dir.filename() == "runtime") {
        return runtime_dir.parent_path();
    }
    return runtime_dir.parent_path();
}

struct LegacyStoragePath {
    const char* component;
    const char* relative_path;
};

constexpr std::array<LegacyStoragePath, 19> kLegacyStoragePaths{{
    {"runtime", "storage_manifest.tsv"},
    {"auth_sessions", "auth_sessions.tsv"},
    {"auth_profiles", "auth_profiles.tsv"},
    {"execution_cache", "execution_cache.tsv"},
    {"trust_identities", "trust/identities.tsv"},
    {"trust_allowlist", "trust/allowlist.tsv"},
    {"trust_invites", "trust/invites.tsv"},
    {"trust_roles", "trust/roles.tsv"},
    {"trust_approvals", "trust/approvals.tsv"},
    {"scheduler_tasks", "scheduler/tasks.tsv"},
    {"scheduler_runs", "scheduler/runs.tsv"},
    {"memory_task_log", "memory/task_log.tsv"},
    {"memory_step_log", "memory/step_log.tsv"},
    {"memory_lessons", "memory/lessons.tsv"},
    {"memory_workflows", "memory/workflows.tsv"},
    {"memory_workflow_candidates", "memory/workflow_candidates.tsv"},
    {"memory_skill_stats", "memory/skill_stats.tsv"},
    {"memory_agent_stats", "memory/agent_stats.tsv"},
    {"audit_log", "audit.log"},
}};

std::vector<std::filesystem::path> LegacyRelativePathsFor(const std::string& component) {
    std::vector<std::filesystem::path> paths;
    for (const auto& legacy : kLegacyStoragePaths) {
        if (component == legacy.component) {
            paths.emplace_back(legacy.relative_path);
        }
    }
    return paths;
}

bool MoveFileIfPresent(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (source.empty() || destination.empty() || source == destination) {
        return false;
    }
    if (!std::filesystem::exists(source) || !std::filesystem::is_regular_file(source)) {
        return false;
    }
    if (std::filesystem::exists(destination)) {
        return false;
    }

    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path());
    }

    std::error_code rename_error;
    std::filesystem::rename(source, destination, rename_error);
    if (!rename_error) {
        return true;
    }

    std::error_code copy_error;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, copy_error);
    if (copy_error) {
        return false;
    }

    std::error_code remove_error;
    std::filesystem::remove(source, remove_error);
    return true;
}

}  // namespace

StorageVersionStore::StorageVersionStore(std::filesystem::path manifest_path)
    : manifest_path_(std::move(manifest_path)) {
    load();
}

void StorageVersionStore::ensure_current() {
    const auto expected = DefaultStorageVersionEntries();
    if (entries_ == expected) {
        return;
    }
    entries_ = expected;
    flush();
}

StorageMigrationResult StorageVersionStore::migrate_to_current() {
    const auto expected = DefaultStorageVersionEntries();
    StorageMigrationResult result;
    const auto workspace_root = WorkspaceRootFromManifest(manifest_path_);

    for (const auto& expected_entry : expected) {
        const auto destination_path = workspace_root / expected_entry.relative_path;
        for (const auto& legacy_relative_path : LegacyRelativePathsFor(expected_entry.component)) {
            const auto source_path = workspace_root / legacy_relative_path;
            if (MoveFileIfPresent(source_path, destination_path)) {
                result.changed = true;
                result.migrated_files += 1;
                break;
            }
        }
    }

    if (entries_ == expected) {
        if (result.changed) {
            flush();
        }
        return result;
    }

    result.created_manifest = entries_.empty();
    result.changed = true;

    for (const auto& expected_entry : expected) {
        const auto it = std::find(entries_.begin(), entries_.end(), expected_entry);
        if (it == entries_.end()) {
            result.updated_entries += 1;
        }
    }

    entries_ = expected;
    flush();
    return result;
}

std::vector<StorageVersionEntry> StorageVersionStore::list() const {
    return entries_;
}

const std::filesystem::path& StorageVersionStore::manifest_path() const {
    return manifest_path_;
}

void StorageVersionStore::load() {
    entries_.clear();
    std::ifstream input(manifest_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 4) {
            continue;
        }
        entries_.push_back(StorageVersionEntry{
            .component = parts[0],
            .format = parts[1],
            .version = parts[2],
            .relative_path = parts[3],
        });
    }
}

void StorageVersionStore::flush() const {
    std::ostringstream output;
    for (const auto& entry : entries_) {
        output
            << EncodeField(entry.component) << kDelimiter
            << EncodeField(entry.format) << kDelimiter
            << EncodeField(entry.version) << kDelimiter
            << EncodeField(entry.relative_path)
            << '\n';
    }
    WriteFileAtomically(manifest_path_, output.str());
}

std::vector<StorageVersionEntry> DefaultStorageVersionEntries() {
    return {
        {"runtime", "manifest.tsv", "1", "runtime/storage_manifest.tsv"},
        {"auth_sessions", "tsv", "1", "runtime/auth_sessions.tsv"},
        {"auth_profiles", "tsv", "1", "runtime/auth_profiles.tsv"},
        {"execution_cache", "tsv", "1", "runtime/execution_cache.tsv"},
        {"plugin_host", "tsv", "1", "runtime/plugin_host.tsv"},
        {"trust_identities", "tsv", "1", "runtime/trust/identities.tsv"},
        {"trust_allowlist", "tsv", "1", "runtime/trust/allowlist.tsv"},
        {"trust_invites", "tsv", "1", "runtime/trust/invites.tsv"},
        {"trust_roles", "tsv", "1", "runtime/trust/roles.tsv"},
        {"trust_approvals", "tsv", "1", "runtime/trust/approvals.tsv"},
        {"scheduler_tasks", "tsv", "1", "runtime/scheduler/tasks.tsv"},
        {"scheduler_runs", "tsv", "1", "runtime/scheduler/runs.tsv"},
        {"memory_task_log", "tsv", "1", "runtime/memory/task_log.tsv"},
        {"memory_step_log", "tsv", "1", "runtime/memory/step_log.tsv"},
        {"memory_lessons", "tsv", "1", "runtime/memory/lessons.tsv"},
        {"memory_workflows", "tsv", "1", "runtime/memory/workflows.tsv"},
        {"memory_workflow_candidates", "tsv", "1", "runtime/memory/workflow_candidates.tsv"},
        {"memory_skill_stats", "tsv", "1", "runtime/memory/skill_stats.tsv"},
        {"memory_agent_stats", "tsv", "1", "runtime/memory/agent_stats.tsv"},
        {"audit_log", "jsonl", "1", "runtime/audit.log"},
    };
}

}  // namespace agentos
