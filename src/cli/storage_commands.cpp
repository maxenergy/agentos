#include "cli/storage_commands.hpp"

#include "storage/storage_backend.hpp"
#include "storage/storage_export.hpp"
#include "storage/storage_policy.hpp"
#include "storage/storage_transaction.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        const std::string arg = argv[index];
        const auto delimiter = arg.find('=');
        if (delimiter == std::string::npos) {
            options[arg] = "true";
        } else {
            options[arg.substr(0, delimiter)] = arg.substr(delimiter + 1);
        }
    }
    return options;
}

void PrintStorageUsage() {
    std::cerr
        << "storage commands:\n"
        << "  agentos storage status\n"
        << "  agentos storage verify [src=<directory>] [strict=true]\n"
        << "  agentos storage backups\n"
        << "  agentos storage restore-backup name=<backup_name>\n"
        << "  agentos storage migrate\n"
        << "  agentos storage export dest=<directory>\n"
        << "  agentos storage import src=<directory>\n"
        << "  agentos storage recover\n"
        << "  agentos storage compact [target=all|memory|scheduler|audit]\n";
}

std::filesystem::path WorkspaceRootFromManifest(const StorageVersionStore& storage_version_store) {
    return storage_version_store.manifest_path().parent_path().filename() == "runtime"
        ? storage_version_store.manifest_path().parent_path().parent_path()
        : std::filesystem::current_path();
}

void PrintStorageStatus(const StorageBackend& storage_backend, const std::filesystem::path& workspace) {
    const auto policy = CurrentStoragePolicy();
    std::cout
        << "decision_id=" << policy.decision_id
        << " backend=" << policy.backend
        << " target_backend=" << policy.target_backend
        << " migration_status=" << policy.migration_status
        << " rationale=\"" << policy.rationale << "\""
        << " revisit_trigger=\"" << policy.revisit_trigger << "\""
        << " migration_boundary=\"" << policy.migration_boundary << "\""
        << " required_interface=\"" << policy.required_interface << "\""
        << " compatibility_contract=\"" << policy.compatibility_contract << "\""
        << '\n';

    for (const auto& entry : storage_backend.manifest_status().entries) {
        const auto path = workspace / entry.relative_path;
        std::uintmax_t bytes = 0;
        bool exists = false;
        std::size_t lines = 0;
        std::error_code error;
        exists = std::filesystem::exists(path, error) && !error;
        if (exists && std::filesystem::is_regular_file(path, error) && !error) {
            bytes = std::filesystem::file_size(path, error);
            std::ifstream input(path, std::ios::binary);
            std::string line;
            while (std::getline(input, line)) {
                lines += 1;
            }
        }
        std::cout
            << entry.component
            << " format=" << entry.format
            << " version=" << entry.version
            << " path=" << entry.relative_path
            << " exists=" << (exists ? "true" : "false")
            << " bytes=" << bytes
            << " lines=" << lines
            << '\n';
    }
}

int VerifyStorageState(
    const StorageBackend& storage_backend,
    const std::filesystem::path& root,
    const bool strict) {
    const auto workspace = !root.empty() ? root : std::filesystem::current_path();
    const auto manifest_status = storage_backend.manifest_status();
    const auto verify_result = storage_backend.verify_manifest();

    std::size_t total = 0;
    std::size_t missing = 0;
    std::size_t non_regular = 0;
    for (const auto& entry : manifest_status.entries) {
        ++total;
        bool exists = true;
        bool regular = true;
        for (const auto& diagnostic : verify_result.diagnostics) {
            if (diagnostic.relative_path != entry.relative_path) {
                continue;
            }
            if (diagnostic.code == "MissingManifestFile") {
                exists = false;
                regular = false;
            } else if (diagnostic.code == "NonRegularManifestFile") {
                regular = false;
            }
        }
        if (!exists) {
            ++missing;
        } else if (!regular) {
            ++non_regular;
        }
        std::cout
            << "storage_verify component=" << entry.component
            << " path=" << entry.relative_path
            << " exists=" << (exists ? "true" : "false")
            << " regular=" << (regular ? "true" : "false")
            << '\n';
    }

    const bool ok = !strict || (missing == 0 && non_regular == 0);
    std::cout
        << "storage_verify_summary"
        << " total=" << total
        << " missing=" << missing
        << " non_regular=" << non_regular
        << " strict=" << (strict ? "true" : "false")
        << " root=\"" << workspace.string() << "\""
        << " ok=" << (ok ? "true" : "false")
        << '\n';
    return ok ? 0 : 1;
}

int PrintImportBackups(const StorageVersionStore& storage_version_store) {
    struct BackupRecord {
        std::string name;
        std::filesystem::path path;
        std::size_t files = 0;
        std::uintmax_t bytes = 0;
    };

    const auto workspace = WorkspaceRootFromManifest(storage_version_store);
    const auto backup_root = workspace / "runtime" / ".import_backups";
    std::vector<BackupRecord> records;
    std::error_code error;
    if (std::filesystem::exists(backup_root, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(backup_root, error)) {
            if (error) {
                break;
            }
            std::error_code entry_error;
            if (!entry.is_directory(entry_error) || entry_error) {
                continue;
            }

            BackupRecord record;
            record.name = entry.path().filename().string();
            record.path = std::filesystem::relative(entry.path(), workspace, entry_error);
            if (entry_error) {
                record.path = entry.path();
            }

            std::error_code walk_error;
            for (const auto& child : std::filesystem::recursive_directory_iterator(entry.path(), walk_error)) {
                if (walk_error) {
                    break;
                }
                std::error_code child_error;
                if (!child.is_regular_file(child_error) || child_error) {
                    continue;
                }
                const auto size = child.file_size(child_error);
                if (!child_error) {
                    ++record.files;
                    record.bytes += size;
                }
            }
            records.push_back(record);
        }
    }

    std::sort(records.begin(), records.end(), [](const BackupRecord& left, const BackupRecord& right) {
        return left.name < right.name;
    });

    std::size_t total_files = 0;
    std::uintmax_t total_bytes = 0;
    for (const auto& record : records) {
        total_files += record.files;
        total_bytes += record.bytes;
        std::cout
            << "storage_backup"
            << " name=" << record.name
            << " path=" << record.path.generic_string()
            << " files=" << record.files
            << " bytes=" << record.bytes
            << '\n';
    }

    std::cout
        << "storage_backups_summary"
        << " count=" << records.size()
        << " files=" << total_files
        << " bytes=" << total_bytes
        << '\n';
    return 0;
}

bool IsSafeBackupName(const std::string& name) {
    return !name.empty()
        && name != "."
        && name != ".."
        && name.find('/') == std::string::npos
        && name.find('\\') == std::string::npos;
}

}  // namespace

int RunStorageCommand(
    StorageVersionStore& storage_version_store,
    SessionStore& session_store,
    AuthProfileStore& auth_profile_store,
    ExecutionCache& execution_cache,
    MemoryManager& memory_manager,
    Scheduler& scheduler,
    AuditLogger& audit_logger,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintStorageUsage();
        return 1;
    }

    const auto command = std::string(argv[2]);
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto workspace = WorkspaceRootFromManifest(storage_version_store);
    TsvStorageBackend storage_backend(workspace);
    if (command == "status") {
        storage_backend.migrate();
        PrintStorageStatus(storage_backend, workspace);
        return 0;
    }

    if (command == "verify") {
        const auto source = options.contains("src") ? std::filesystem::path(options.at("src")) : std::filesystem::path{};
        const bool strict = options.contains("strict") && options.at("strict") == "true";
        if (!source.empty()) {
            TsvStorageBackend source_backend(source);
            if (source_backend.manifest_status().entries.empty()) {
                std::cerr << "src does not contain a readable runtime/storage_manifest.tsv\n";
                return 1;
            }
            return VerifyStorageState(source_backend, source, strict);
        }

        storage_backend.migrate();
        return VerifyStorageState(storage_backend, workspace, strict);
    }

    if (command == "backups") {
        return PrintImportBackups(storage_version_store);
    }

    if (command == "restore-backup") {
        const auto name = options.contains("name") ? options.at("name") : "";
        if (!IsSafeBackupName(name)) {
            std::cerr << "name must be a backup directory name\n";
            return 1;
        }

        const auto workspace = WorkspaceRootFromManifest(storage_version_store);
        const auto backup_root = workspace / "runtime" / ".import_backups" / name;
        std::error_code error;
        if (!std::filesystem::exists(backup_root, error) || error || !std::filesystem::is_directory(backup_root, error) || error) {
            std::cerr << "backup not found: " << name << '\n';
            return 1;
        }

        try {
            const auto result = storage_backend.import_state(backup_root);
            storage_version_store.ensure_current();
            std::cout
                << "restored_files=" << result.imported_files
                << " backed_up_files=" << result.backed_up_files
                << " backup=" << name
                << " source=" << result.source_root.string()
                << " current_backup=" << result.backup_root.string()
                << '\n';
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "restore-backup failed: " << exception.what() << '\n';
            return 1;
        }
    }

    if (command == "migrate") {
        const auto result = storage_backend.migrate();
        std::vector<std::string> normalized_targets;
        if (!memory_manager.storage_dir().empty()) {
            const auto task_log_path = memory_manager.storage_dir() / "task_log.tsv";
            const auto step_log_path = memory_manager.storage_dir() / "step_log.tsv";
            if (std::filesystem::exists(task_log_path) || std::filesystem::exists(step_log_path)) {
                memory_manager.compact_logs();
                normalized_targets.push_back("memory_logs");
            }

            const auto workflow_store_path = memory_manager.workflow_store().store_path();
            if (!workflow_store_path.empty() && std::filesystem::exists(workflow_store_path)) {
                memory_manager.workflow_store().compact();
                normalized_targets.push_back("workflow_store");
            }

            const auto lesson_store_path = memory_manager.lesson_store().store_path();
            if (!lesson_store_path.empty() && std::filesystem::exists(lesson_store_path)) {
                memory_manager.lesson_store().compact();
                normalized_targets.push_back("lesson_store");
            }
        }

        if (!session_store.store_path().empty() && std::filesystem::exists(session_store.store_path())) {
            session_store.compact();
            normalized_targets.push_back("auth_sessions");
        }
        if (!auth_profile_store.store_path().empty() && std::filesystem::exists(auth_profile_store.store_path())) {
            auth_profile_store.compact();
            normalized_targets.push_back("auth_profiles");
        }
        if (!execution_cache.cache_path().empty() && std::filesystem::exists(execution_cache.cache_path())) {
            execution_cache.compact();
            normalized_targets.push_back("execution_cache");
        }

        if (std::filesystem::exists(scheduler.store_path())) {
            scheduler.compact_tasks();
            normalized_targets.push_back("scheduler_tasks");
        }
        if (std::filesystem::exists(scheduler.history_path())) {
            scheduler.compact_history();
            normalized_targets.push_back("scheduler_runs");
        }

        std::cout
            << "changed=" << (result.changed ? "true" : "false")
            << " created_manifest=" << (result.created_manifest ? "true" : "false")
            << " updated_entries=" << result.updated_entries
            << " migrated_files=" << result.migrated_files
            << " normalized_targets=";
        for (std::size_t index = 0; index < normalized_targets.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << normalized_targets[index];
        }
        std::cout << '\n';
        return 0;
    }

    if (command == "export") {
        const auto destination = options.contains("dest") ? options.at("dest") : "";
        if (destination.empty()) {
            std::cerr << "dest is required\n";
            return 1;
        }

        storage_backend.migrate();
        const auto result = storage_backend.export_state(std::filesystem::path(destination));
        std::cout
            << "exported_files=" << result.exported_files
            << " destination=" << result.destination_root.string()
            << '\n';
        return 0;
    }

    if (command == "import") {
        const auto source = options.contains("src") ? options.at("src") : "";
        if (source.empty()) {
            std::cerr << "src is required\n";
            return 1;
        }

        const auto result = storage_backend.import_state(std::filesystem::path(source));
        storage_version_store.ensure_current();
        std::cout
            << "imported_files=" << result.imported_files
            << " backed_up_files=" << result.backed_up_files
            << " source=" << result.source_root.string()
            << " backup=" << result.backup_root.string()
            << '\n';
        return 0;
    }

    if (command == "recover") {
        const auto result = storage_backend.recover_transactions();
        std::cout
            << "committed_replayed=" << result.committed_replayed
            << " rolled_back=" << result.rolled_back
            << " failed=" << result.failed
            << " files_applied=" << result.files_applied
            << '\n';
        return 0;
    }

    if (command == "compact") {
        const auto target = options.contains("target") ? options.at("target") : "all";
        std::vector<std::string> compacted;

        if (target == "all" || target == "memory") {
            memory_manager.compact_logs();
            compacted.push_back("task_log");
            compacted.push_back("step_log");
        }
        if (target == "all" || target == "scheduler") {
            scheduler.compact_history();
            compacted.push_back("scheduler_runs");
        }
        if (target == "all" || target == "audit") {
            audit_logger.compact_log(memory_manager.storage_dir());
            compacted.push_back("audit_log");
        }
        if (compacted.empty()) {
            std::cerr << "target must be one of: all, memory, scheduler, audit\n";
            return 1;
        }

        storage_backend.compact(target);
        storage_backend.migrate();
        std::cout << "compacted=";
        for (std::size_t index = 0; index < compacted.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << compacted[index];
        }
        std::cout << '\n';
        return 0;
    }

    PrintStorageUsage();
    return 1;
}

}  // namespace agentos
