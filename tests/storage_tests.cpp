#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "memory/lesson_store.hpp"
#include "memory/memory_manager.hpp"
#include "memory/workflow_store.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/storage_export.hpp"
#include "storage/storage_policy.hpp"
#include "storage/storage_transaction.hpp"
#include "storage/storage_backend.hpp"
#include "storage/storage_version_store.hpp"
#include "auth/session_store.hpp"
#include "utils/atomic_file.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::size_t CountFields(const std::string& line, const char delimiter = '\t') {
    if (line.empty()) {
        return 0;
    }

    std::size_t count = 1;
    for (const char ch : line) {
        if (ch == delimiter) {
            count += 1;
        }
    }
    return count;
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_storage_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void TestAtomicFileWriteReplacesContent(const std::filesystem::path& workspace) {
    const auto file_path = workspace / "atomic" / "state.tsv";
    agentos::WriteFileAtomically(file_path, "first\n");
    agentos::WriteFileAtomically(file_path, "second\n");

    std::ifstream input(file_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Expect(buffer.str() == "second\n", "atomic file write should replace existing content");

    for (const auto& entry : std::filesystem::directory_iterator(file_path.parent_path())) {
        Expect(entry.path().filename().string().find(".tmp.") == std::string::npos,
            "atomic file write should not leave temporary files after success");
        Expect(entry.path().filename().string().find(".lock") == std::string::npos,
            "atomic file write should release the single-writer lock after success");
    }
}

void TestAppendLineToFileAppendsAndReleasesLock(const std::filesystem::path& workspace) {
    const auto file_path = workspace / "append" / "events.log";
    agentos::AppendLineToFile(file_path, "alpha");
    agentos::AppendLineToFile(file_path, "beta");

    std::ifstream input(file_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Expect(buffer.str() == "alpha\nbeta\n", "append helper should preserve existing lines and append new ones");

    for (const auto& entry : std::filesystem::directory_iterator(file_path.parent_path())) {
        Expect(entry.path().filename().string().find(".lock") == std::string::npos,
            "append helper should release the single-writer lock after success");
    }
}

void TestAppendLineToFileRecoversPendingAppend(const std::filesystem::path& workspace) {
    const auto file_path = workspace / "append_recovery" / "events.log";
    std::filesystem::create_directories(file_path.parent_path());
    {
        std::ofstream output(file_path, std::ios::binary);
        output << "alpha\n";
    }
    {
        std::ofstream recovery(file_path.parent_path() / "events.log.append.tmp.manual", std::ios::binary);
        recovery << "6\nbeta";
    }

    agentos::AppendLineToFile(file_path, "gamma");

    std::ifstream input(file_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Expect(buffer.str() == "alpha\nbeta\ngamma\n",
        "append helper should recover pending append intents before writing new lines");

    for (const auto& entry : std::filesystem::directory_iterator(file_path.parent_path())) {
        Expect(entry.path().filename().string().find(".append.tmp.") == std::string::npos,
            "append helper should remove recovered append intent files");
    }
}

void TestAppendLineToFileDoesNotDuplicateRecoveredAppend(const std::filesystem::path& workspace) {
    const auto file_path = workspace / "append_recovery_after_write" / "events.log";
    std::filesystem::create_directories(file_path.parent_path());
    {
        std::ofstream output(file_path, std::ios::binary);
        output << "alpha\nbeta\n";
    }
    {
        std::ofstream recovery(file_path.parent_path() / "events.log.append.tmp.manual", std::ios::binary);
        recovery << "6\nbeta";
    }

    agentos::AppendLineToFile(file_path, "gamma");

    std::ifstream input(file_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Expect(buffer.str() == "alpha\nbeta\ngamma\n",
        "append helper should not duplicate an append already present at the recovery offset");
}

void TestAppendLineToFileConcurrentWriters(const std::filesystem::path& workspace) {
    const auto file_path = workspace / "append_concurrent" / "events.log";
    constexpr int thread_count = 8;
    constexpr int lines_per_thread = 20;

    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([file_path, thread_index]() {
            for (int line_index = 0; line_index < lines_per_thread; ++line_index) {
                agentos::AppendLineToFile(
                    file_path,
                    "thread-" + std::to_string(thread_index) + "-line-" + std::to_string(line_index));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::ifstream input(file_path, std::ios::binary);
    std::string line;
    std::set<std::string> lines;
    while (std::getline(input, line)) {
        lines.insert(line);
    }

    Expect(lines.size() == static_cast<std::size_t>(thread_count * lines_per_thread),
        "concurrent append should persist every unique line exactly once");
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        for (int line_index = 0; line_index < lines_per_thread; ++line_index) {
            const auto expected = "thread-" + std::to_string(thread_index) + "-line-" + std::to_string(line_index);
            Expect(lines.contains(expected), "concurrent append should not lose lines");
        }
    }

    for (const auto& entry : std::filesystem::directory_iterator(file_path.parent_path())) {
        const auto name = entry.path().filename().string();
        Expect(name.find(".lock") == std::string::npos, "concurrent append should not leave lock files");
        Expect(name.find(".append.tmp.") == std::string::npos, "concurrent append should not leave recovery intents");
    }
}

void TestAtomicFileWriteConcurrentWriters(const std::filesystem::path& workspace) {
    const auto file_path = workspace / "atomic_concurrent" / "state.tsv";
    constexpr int thread_count = 8;
    constexpr int writes_per_thread = 10;

    std::set<std::string> expected_values;
    std::vector<std::thread> threads;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        for (int write_index = 0; write_index < writes_per_thread; ++write_index) {
            expected_values.insert(
                "thread-" + std::to_string(thread_index) + "-write-" + std::to_string(write_index) + "\n");
        }
        threads.emplace_back([file_path, thread_index]() {
            for (int write_index = 0; write_index < writes_per_thread; ++write_index) {
                agentos::WriteFileAtomically(
                    file_path,
                    "thread-" + std::to_string(thread_index) + "-write-" + std::to_string(write_index) + "\n");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto final_content = ReadFile(file_path);
    Expect(expected_values.contains(final_content),
        "concurrent atomic writes should leave one complete writer payload");

    for (const auto& entry : std::filesystem::directory_iterator(file_path.parent_path())) {
        const auto name = entry.path().filename().string();
        Expect(name.find(".lock") == std::string::npos, "concurrent atomic writes should not leave lock files");
        Expect(name.find(".tmp.") == std::string::npos, "concurrent atomic writes should not leave temporary files");
    }
}

void TestStorageTransactionCommitsMultipleFiles(const std::filesystem::path& workspace) {
    const auto runtime_dir = workspace / "transaction_commit" / "runtime";
    const auto first = runtime_dir / "state" / "first.tsv";
    const auto second = runtime_dir / "state" / "second.tsv";

    const auto applied = agentos::WriteFilesTransactionally(runtime_dir, "commit-smoke", {
        agentos::StorageTransactionWrite{.target_path = first, .content = "first\n"},
        agentos::StorageTransactionWrite{.target_path = second, .content = "second\n"},
    });

    Expect(applied == 2, "storage transaction should report applied files");
    Expect(ReadFile(first) == "first\n", "storage transaction should write the first target");
    Expect(ReadFile(second) == "second\n", "storage transaction should write the second target");
    Expect(!std::filesystem::exists(runtime_dir / ".transactions" / "commit-smoke"),
        "storage transaction should remove completed transaction state");
}

void TestStorageTransactionRollsBackUncommittedPrepare(const std::filesystem::path& workspace) {
    const auto runtime_dir = workspace / "transaction_rollback" / "runtime";
    const auto target = runtime_dir / "state.tsv";
    agentos::PrepareStorageTransaction(runtime_dir, "rollback-smoke", {
        agentos::StorageTransactionWrite{.target_path = target, .content = "prepared\n"},
    });

    const auto recovered = agentos::RecoverStorageTransactions(runtime_dir);

    Expect(recovered.rolled_back == 1, "storage recovery should roll back uncommitted prepared transactions");
    Expect(recovered.committed_replayed == 0, "storage recovery should not replay uncommitted transactions");
    Expect(!std::filesystem::exists(target), "storage recovery should not apply uncommitted prepared writes");
    Expect(!std::filesystem::exists(runtime_dir / ".transactions" / "rollback-smoke"),
        "storage recovery should remove rolled-back transaction state");
}

void TestStorageTransactionRecoversCommittedPrepare(const std::filesystem::path& workspace) {
    const auto runtime_dir = workspace / "transaction_recover" / "runtime";
    const auto first = runtime_dir / "state" / "first.tsv";
    const auto second = runtime_dir / "state" / "second.tsv";
    agentos::PrepareStorageTransaction(runtime_dir, "recover-smoke", {
        agentos::StorageTransactionWrite{.target_path = first, .content = "first-recovered\n"},
        agentos::StorageTransactionWrite{.target_path = second, .content = "second-recovered\n"},
    });
    agentos::MarkStorageTransactionCommitted(runtime_dir, "recover-smoke");

    const auto recovered = agentos::RecoverStorageTransactions(runtime_dir);

    Expect(recovered.committed_replayed == 1, "storage recovery should replay committed prepared transactions");
    Expect(recovered.files_applied == 2, "storage recovery should report replayed files");
    Expect(ReadFile(first) == "first-recovered\n", "storage recovery should apply first committed write");
    Expect(ReadFile(second) == "second-recovered\n", "storage recovery should apply second committed write");
    Expect(!std::filesystem::exists(runtime_dir / ".transactions" / "recover-smoke"),
        "storage recovery should remove replayed transaction state");
}

void TestStorageTransactionSkipsCorruptCommittedPrepare(const std::filesystem::path& workspace) {
    const auto runtime_dir = workspace / "transaction_corrupt_recover" / "runtime";
    const auto good_target = runtime_dir / "state" / "good.tsv";
    const auto corrupt_target = runtime_dir / "state" / "corrupt.tsv";
    const auto committed_target = runtime_dir / "state" / "committed.tsv";

    agentos::PrepareStorageTransaction(runtime_dir, "corrupt-committed", {
        agentos::StorageTransactionWrite{.target_path = good_target, .content = "should-not-partially-apply\n"},
        agentos::StorageTransactionWrite{.target_path = corrupt_target, .content = "missing-staged-payload\n"},
    });
    std::filesystem::remove(runtime_dir / ".transactions" / "corrupt-committed" / "file_1.data");
    agentos::MarkStorageTransactionCommitted(runtime_dir, "corrupt-committed");

    agentos::PrepareStorageTransaction(runtime_dir, "valid-committed", {
        agentos::StorageTransactionWrite{.target_path = committed_target, .content = "committed-after-corrupt\n"},
    });
    agentos::MarkStorageTransactionCommitted(runtime_dir, "valid-committed");

    const auto recovered = agentos::RecoverStorageTransactions(runtime_dir);

    Expect(recovered.failed == 1, "storage recovery should count corrupt committed transactions as failed");
    Expect(recovered.committed_replayed == 1, "storage recovery should continue replaying later valid transactions");
    Expect(recovered.files_applied == 1, "storage recovery should report files applied from valid transactions only");
    Expect(!std::filesystem::exists(good_target),
        "storage recovery should not partially apply corrupt transactions before detecting missing staged files");
    Expect(!std::filesystem::exists(corrupt_target),
        "storage recovery should not apply corrupt transaction targets");
    Expect(ReadFile(committed_target) == "committed-after-corrupt\n",
        "storage recovery should apply valid committed transactions after a corrupt one");
    Expect(!std::filesystem::exists(runtime_dir / ".transactions" / "corrupt-committed"),
        "storage recovery should remove corrupt committed transaction state after accounting for failure");
    Expect(!std::filesystem::exists(runtime_dir / ".transactions" / "valid-committed"),
        "storage recovery should remove valid committed transaction state");
}

void TestTsvStorageBackendCoreCapabilities(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "storage_backend_tsv";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace);

    agentos::TsvStorageBackend backend(isolated_workspace);

    const auto state_path = isolated_workspace / "runtime" / "backend" / "state.tsv";
    backend.atomic_replace(state_path, "first\n");
    backend.atomic_replace(state_path, "second\n");
    Expect(ReadFile(state_path) == "second\n", "TSV StorageBackend atomic_replace should replace content");

    const auto events_path = isolated_workspace / "runtime" / "backend" / "events.log";
    backend.append_line(events_path, "alpha");
    backend.append_line(events_path, "beta");
    Expect(ReadFile(events_path) == "alpha\nbeta\n", "TSV StorageBackend append_line should append lines");

    const auto transaction_path = isolated_workspace / "runtime" / "backend" / "transaction.tsv";
    backend.prepare_transaction("backend-smoke", {
        agentos::StorageTransactionWrite{.target_path = transaction_path, .content = "transactional\n"},
    });
    Expect(backend.commit_prepared_transaction("backend-smoke") == 1,
        "TSV StorageBackend should commit prepared transaction writes");
    Expect(ReadFile(transaction_path) == "transactional\n",
        "TSV StorageBackend transaction should apply the target content");
    const auto recovered = backend.recover_transactions();
    Expect(recovered.committed_replayed == 0 && recovered.rolled_back == 0,
        "TSV StorageBackend recovery should report no leftover transaction state after commit");

    const auto migration = backend.migrate();
    Expect(migration.changed && migration.created_manifest,
        "TSV StorageBackend migrate should create the runtime storage manifest");
    const auto status = backend.manifest_status();
    Expect(status.current, "TSV StorageBackend manifest_status should report the current manifest after migrate");
    Expect(!status.entries.empty(), "TSV StorageBackend manifest_status should expose manifest entries");

    const auto verify = backend.verify_manifest();
    Expect(!verify.valid, "TSV StorageBackend verify_manifest should report missing manifested files");
    Expect(verify.checked_files == static_cast<int>(status.entries.size()),
        "TSV StorageBackend verify_manifest should check every manifest entry");
    Expect(!verify.diagnostics.empty() && verify.diagnostics.front().code == "MissingManifestFile",
        "TSV StorageBackend verify_manifest should return audit-safe diagnostics");

    const auto compact = backend.compact("memory");
    Expect(compact.success, "TSV StorageBackend compact seam should be callable");
    Expect(!compact.diagnostics.empty() && compact.diagnostics.front().code == "CompactionDelegated",
        "TSV StorageBackend compact seam should explain delegated compaction");
}

void TestStorageVersionStorePersistsManifest(const std::filesystem::path& workspace) {
    const auto manifest_path = workspace / "runtime" / "storage_manifest.tsv";
    agentos::StorageVersionStore store(manifest_path);
    store.ensure_current();

    agentos::StorageVersionStore reloaded(manifest_path);
    const auto entries = reloaded.list();
    Expect(!entries.empty(), "storage version manifest should be created on ensure_current");
    Expect(entries.front().component == "runtime", "storage manifest should include runtime manifest entry first");
    Expect(entries.front().relative_path == "runtime/storage_manifest.tsv",
        "storage manifest should store repo-relative runtime path");
    const auto plugin_host = std::find_if(entries.begin(), entries.end(), [](const agentos::StorageVersionEntry& entry) {
        return entry.component == "plugin_host";
    });
    Expect(plugin_host != entries.end() && plugin_host->relative_path == "runtime/plugin_host.tsv",
        "storage manifest should include plugin host runtime configuration");
}

void TestStorageVersionStoreMigratesMissingManifest(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "storage_migrate_missing";
    std::filesystem::remove_all(isolated_workspace);

    agentos::StorageVersionStore store(isolated_workspace / "runtime" / "storage_manifest.tsv");
    const auto result = store.migrate_to_current();
    const auto entries = store.list();

    Expect(result.changed, "storage migrate should change state when manifest is missing");
    Expect(result.created_manifest, "storage migrate should mark missing manifest creation");
    Expect(result.updated_entries > 0, "storage migrate should report populated manifest entries");
    Expect(!entries.empty(), "storage migrate should populate manifest entries");
}

void TestStorageVersionStoreMigratesLegacyManifest(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "storage_migrate_legacy";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime");

    {
        std::ofstream output(isolated_workspace / "runtime" / "storage_manifest.tsv", std::ios::binary);
        output << "runtime\tmanifest.tsv\t0\truntime/storage_manifest.tsv\n";
    }

    agentos::StorageVersionStore store(isolated_workspace / "runtime" / "storage_manifest.tsv");
    const auto result = store.migrate_to_current();
    const auto entries = store.list();

    Expect(result.changed, "storage migrate should change state when manifest entries are outdated");
    Expect(!result.created_manifest, "storage migrate should not mark legacy manifest as newly created");
    Expect(result.updated_entries > 0, "storage migrate should report updated legacy entries");
    Expect(entries.size() > 1, "storage migrate should replace legacy manifest with current entries");
}

void TestStorageVersionStoreMigratesLegacyPaths(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "storage_migrate_paths";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "memory");
    std::filesystem::create_directories(isolated_workspace / "trust");

    {
        std::ofstream output(isolated_workspace / "auth_sessions.tsv", std::ios::binary);
        output << "session-1\tprovider\n";
    }
    {
        std::ofstream output(isolated_workspace / "memory" / "task_log.tsv", std::ios::binary);
        output << "task-1\twrite_file\n";
    }
    {
        std::ofstream output(isolated_workspace / "trust" / "identities.tsv", std::ios::binary);
        output << "phone\tdevice-1\n";
    }
    {
        std::ofstream output(isolated_workspace / "audit.log", std::ios::binary);
        output << "{\"event\":\"task\"}\n";
    }

    agentos::StorageVersionStore store(isolated_workspace / "runtime" / "storage_manifest.tsv");
    const auto result = store.migrate_to_current();

    Expect(result.changed, "storage migrate should change state when legacy storage files are present");
    Expect(result.migrated_files >= 4, "storage migrate should move legacy files into runtime paths");
    Expect(std::filesystem::exists(isolated_workspace / "runtime" / "auth_sessions.tsv"),
        "storage migrate should move auth sessions into runtime");
    Expect(std::filesystem::exists(isolated_workspace / "runtime" / "memory" / "task_log.tsv"),
        "storage migrate should move task log into runtime/memory");
    Expect(std::filesystem::exists(isolated_workspace / "runtime" / "trust" / "identities.tsv"),
        "storage migrate should move trust identities into runtime/trust");
    Expect(std::filesystem::exists(isolated_workspace / "runtime" / "audit.log"),
        "storage migrate should move audit log into runtime");
}

void TestStoragePolicyDecisionDefaultsToTsvMvp() {
    const auto policy = agentos::CurrentStoragePolicy();
    Expect(policy.decision_id == "ADR-STORAGE-001", "storage policy should expose a stable decision id");
    Expect(policy.backend == "tsv-mvp", "storage policy should currently keep TSV as the MVP backend");
    Expect(policy.target_backend == "sqlite", "storage policy should name SQLite as the deferred target backend");
    Expect(policy.migration_status == "deferred", "storage policy should mark SQLite migration as deferred");
    Expect(policy.migration_boundary.find("StorageBackend") != std::string::npos,
        "storage policy should define the interface boundary before a SQLite backend is added");
    Expect(policy.required_interface.find("transaction_prepare_commit_recover") != std::string::npos,
        "storage policy should require transactional recovery in the backend interface");
    Expect(policy.compatibility_contract.find("TSV import") != std::string::npos,
        "storage policy should preserve a TSV migration compatibility contract");
}

void TestWorkflowStoreCompactsLegacyFormat(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "workflow_store_compact_legacy";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "memory");

    const auto store_path = isolated_workspace / "runtime" / "memory" / "workflows.tsv";
    {
        std::ofstream output(store_path, std::ios::binary);
        output << "wf\t1\twrite_file\tread_file,write_file\tmanual\t3\t2\t1\t0.66\t12.5\t88.0\n";
    }

    agentos::WorkflowStore store(store_path);
    store.compact();

    std::ifstream input(store_path, std::ios::binary);
    std::string line;
    std::getline(input, line);

    Expect(CountFields(line) == 19, "workflow store compaction should rewrite legacy rows to the current 19-column schema");
}

void TestSchedulerCompactsLegacyTaskStore(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "scheduler_compact_legacy";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "scheduler");

    const auto store_path = isolated_workspace / "runtime" / "scheduler" / "tasks.tsv";
    {
        std::ofstream output(store_path, std::ios::binary);
        output << "sched-1\t1\t1000\t60\t1\t0\twrite_file\tLegacy schedule\t"
               << isolated_workspace.string()
               << "\tlocal-user\tlegacy-key\t0\t\t\t\t5000\t0\t0\t0\tpath=runtime%2Fnote.txt&content=hello\n";
    }

    agentos::Scheduler scheduler(store_path);
    scheduler.compact_tasks();

    std::ifstream input(store_path, std::ios::binary);
    std::string line;
    std::getline(input, line);

    Expect(CountFields(line) == 28, "scheduler task compaction should rewrite legacy rows to the current 28-column schema");
}

void TestSessionStoreCompactsCurrentFormat(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "session_store_compact";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime");

    agentos::SessionStore store(isolated_workspace / "runtime" / "auth_sessions.tsv");
    store.save(agentos::AuthSession{
        .session_id = "openai-api_key-default",
        .provider = agentos::AuthProviderId::openai,
        .mode = agentos::AuthMode::api_key,
        .profile_name = "default",
        .account_label = "dev",
        .managed_by_agentos = true,
        .managed_by_external_cli = false,
        .refresh_supported = false,
        .headless_compatible = true,
        .access_token_ref = "env:OPENAI_API_KEY",
        .refresh_token_ref = "",
        .metadata = {{"source", "test"}},
    });
    store.compact();

    std::ifstream input(isolated_workspace / "runtime" / "auth_sessions.tsv", std::ios::binary);
    std::string line;
    std::getline(input, line);

    Expect(CountFields(line) == 13, "session store compaction should rewrite rows to the current 13-column schema");
}

void TestExecutionCacheCompactsCurrentFormat(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "execution_cache_compact";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime");

    agentos::ExecutionCache cache(isolated_workspace / "runtime" / "execution_cache.tsv");
    cache.store("demo-key", agentos::TaskRunResult{
        .success = true,
        .summary = "cached result",
        .route_target = "write_file",
        .route_kind = agentos::RouteTargetKind::skill,
        .duration_ms = 7,
    });
    cache.compact();

    std::ifstream input(isolated_workspace / "runtime" / "execution_cache.tsv", std::ios::binary);
    std::string line;
    std::getline(input, line);

    Expect(CountFields(line) == 10, "execution cache compaction should rewrite rows to the current 10-column schema");
}

void TestLessonStoreCompactsCurrentFormat(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "lesson_store_compact";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "memory");

    agentos::LessonStore store(isolated_workspace / "runtime" / "memory" / "lessons.tsv");
    store.save(agentos::LessonRecord{
        .lesson_id = "write_file|write_file|PermissionDenied",
        .task_type = "write_file",
        .target_name = "write_file",
        .error_code = "PermissionDenied",
        .summary = "blocked",
        .occurrence_count = 2,
        .last_task_id = "task-9",
        .enabled = true,
    });
    store.compact();

    std::ifstream input(isolated_workspace / "runtime" / "memory" / "lessons.tsv", std::ios::binary);
    std::string line;
    std::getline(input, line);

    Expect(CountFields(line) == 8, "lesson store compaction should rewrite rows to the current 8-column schema");
}

void TestStorageExportCopiesManifestedFiles(const std::filesystem::path& workspace) {
    const auto runtime_dir = workspace / "runtime";
    std::filesystem::create_directories(runtime_dir / "memory");

    agentos::StorageVersionStore store(runtime_dir / "storage_manifest.tsv");
    store.ensure_current();

    {
        std::ofstream output(runtime_dir / "auth_sessions.tsv", std::ios::binary);
        output << "session\tdata\n";
    }
    {
        std::ofstream output(runtime_dir / "memory" / "task_log.tsv", std::ios::binary);
        output << "task\tentry\n";
    }
    {
        std::ofstream output(runtime_dir / "demo_note.txt", std::ios::binary);
        output << "do not export me\n";
    }

    const auto export_dir = workspace / "storage_export";
    const auto result = agentos::ExportStorageState(workspace, store, export_dir);

    Expect(result.exported_files >= 3, "storage export should copy manifest file and manifested runtime files");
    Expect(std::filesystem::exists(export_dir / "runtime" / "storage_manifest.tsv"),
        "storage export should include storage manifest");
    Expect(std::filesystem::exists(export_dir / "runtime" / "auth_sessions.tsv"),
        "storage export should include manifested auth session file");
    Expect(std::filesystem::exists(export_dir / "runtime" / "memory" / "task_log.tsv"),
        "storage export should include manifested memory task log");
    Expect(!std::filesystem::exists(export_dir / "runtime" / "demo_note.txt"),
        "storage export should skip runtime files that are not in the manifest");
}

void TestStorageImportCopiesManifestedFiles(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "storage_import_isolated";
    std::filesystem::remove_all(isolated_workspace);

    const auto import_source = isolated_workspace / "storage_import_source";
    std::filesystem::create_directories(import_source / "runtime" / "memory");

    agentos::StorageVersionStore source_store(import_source / "runtime" / "storage_manifest.tsv");
    source_store.ensure_current();

    {
        std::ofstream output(import_source / "runtime" / "auth_sessions.tsv", std::ios::binary);
        output << "imported-session\tdata\n";
    }
    {
        std::ofstream output(import_source / "runtime" / "memory" / "task_log.tsv", std::ios::binary);
        output << "imported-task\tentry\n";
    }
    {
        std::ofstream output(import_source / "runtime" / "demo_note.txt", std::ios::binary);
        output << "do not import me\n";
    }

    const auto target_runtime = isolated_workspace / "runtime";
    std::filesystem::create_directories(target_runtime / "memory");
    {
        std::ofstream output(target_runtime / "auth_sessions.tsv", std::ios::binary);
        output << "stale-session\n";
    }

    const auto result = agentos::ImportStorageState(isolated_workspace, import_source);
    Expect(result.imported_files >= 3, "storage import should copy manifest file and manifested runtime files");
    Expect(result.backed_up_files >= 1, "storage import should back up existing manifested target files before overwriting");
    Expect(std::filesystem::exists(result.backup_root / "runtime" / "auth_sessions.tsv"),
        "storage import should place backups under the reported backup root");
    Expect(ReadFile(result.backup_root / "runtime" / "auth_sessions.tsv") == "stale-session\n",
        "storage import backup should preserve the pre-import file contents");

    std::ifstream auth_input(target_runtime / "auth_sessions.tsv", std::ios::binary);
    std::ostringstream auth_buffer;
    auth_buffer << auth_input.rdbuf();
    Expect(auth_buffer.str() == "imported-session\tdata\n",
        "storage import should overwrite manifested files with imported contents");

    Expect(std::filesystem::exists(target_runtime / "memory" / "task_log.tsv"),
        "storage import should copy manifested nested files");
}

void TestMemoryManagerCompactsLogs(const std::filesystem::path& workspace) {
    const auto storage_dir = workspace / "memory_compact_isolated";
    std::filesystem::remove_all(storage_dir);
    std::filesystem::create_directories(storage_dir);

    {
        std::ofstream output(storage_dir / "task_log.tsv", std::ios::binary);
        output << "task-1\tread_file\tobjective\t\t1\t0\t12\tskill\tfile_read\t\t\n";
        output << "broken\n";
    }
    {
        std::ofstream output(storage_dir / "step_log.tsv", std::ios::binary);
        output << "task-1\tskill\tfile_read\t1\t12\t0\tok\t\t\n";
        output << "broken\n";
    }

    agentos::MemoryManager memory_manager(storage_dir);
    memory_manager.compact_logs();

    std::ifstream tasks_input(storage_dir / "task_log.tsv", std::ios::binary);
    std::ostringstream tasks_buffer;
    tasks_buffer << tasks_input.rdbuf();
    Expect(tasks_buffer.str().find("broken") == std::string::npos,
        "memory log compaction should remove malformed task log lines");
    Expect(tasks_buffer.str().find("task-1") != std::string::npos,
        "memory log compaction should preserve parsed task log lines");

    std::ifstream steps_input(storage_dir / "step_log.tsv", std::ios::binary);
    std::ostringstream steps_buffer;
    steps_buffer << steps_input.rdbuf();
    Expect(steps_buffer.str().find("broken") == std::string::npos,
        "memory log compaction should remove malformed step log lines");
    Expect(steps_buffer.str().find("file_read") != std::string::npos,
        "memory log compaction should preserve parsed step log lines");
}

void TestSchedulerCompactsHistory(const std::filesystem::path& workspace) {
    const auto scheduler_dir = workspace / "scheduler_compact_isolated";
    std::filesystem::remove_all(scheduler_dir);
    std::filesystem::create_directories(scheduler_dir);

    {
        std::ofstream output(scheduler_dir / "tasks.tsv", std::ios::binary);
    }
    {
        std::ofstream output(scheduler_dir / "runs.tsv", std::ios::binary);
        output << "schedule-1\ttask-1\t100\t120\t1\t1\t0\tskill\tfile_read\t\t\t20\n";
        output << "broken\n";
    }

    agentos::Scheduler scheduler(scheduler_dir / "tasks.tsv");
    scheduler.compact_history();

    std::ifstream history_input(scheduler_dir / "runs.tsv", std::ios::binary);
    std::ostringstream history_buffer;
    history_buffer << history_input.rdbuf();
    Expect(history_buffer.str().find("broken") == std::string::npos,
        "scheduler history compaction should remove malformed lines");
    Expect(history_buffer.str().find("schedule-1") != std::string::npos,
        "scheduler history compaction should preserve parsed records");
}

void TestAuditLoggerCompactsLog(const std::filesystem::path& workspace) {
    const auto audit_dir = workspace / "audit_compact_isolated";
    std::filesystem::remove_all(audit_dir);
    std::filesystem::create_directories(audit_dir);

    const auto audit_path = audit_dir / "audit.log";
    {
        std::ofstream output(audit_path, std::ios::binary);
        output << "{\"event\":\"task_start\"}\n";
        output << "broken\n";
        output << "{\"event\":\"task_end\"}\n";
    }

    agentos::AuditLogger audit_logger(audit_path);
    audit_logger.compact_log();

    std::ifstream input(audit_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Expect(buffer.str().find("broken") == std::string::npos,
        "audit log compaction should remove malformed lines");
    Expect(buffer.str().find("\"task_start\"") != std::string::npos,
        "audit log compaction should preserve valid JSON object lines");
    Expect(buffer.str().find("\"task_end\"") != std::string::npos,
        "audit log compaction should preserve later valid JSON object lines");
}

void TestAuditLoggerRebuildsSchedulerRunEvents(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "audit_scheduler_recovery_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "memory");
    std::filesystem::create_directories(isolated_workspace / "runtime" / "scheduler");

    const auto audit_path = isolated_workspace / "runtime" / "audit.log";
    {
        std::ofstream audit_output(audit_path, std::ios::binary);
        audit_output << "{\"ts\":\"2026-01-01T00:00:00Z\",\"event\":\"scheduler_run\","
                     << "\"schedule_id\":\"stale\",\"task_id\":\"stale.run\"}\n";
        audit_output << "{\"ts\":\"2026-01-01T00:00:01Z\",\"event\":\"trust\","
                     << "\"action\":\"pair\",\"success\":true}\n";
    }
    {
        std::ofstream runs_output(isolated_workspace / "runtime" / "scheduler" / "runs.tsv", std::ios::binary);
        runs_output << "sched-1\tsched-1.run-1\t1767225600000\t1767225600123\t1\t1\t1\t"
                    << "skill\tfile_write\t\t\t123\n";
    }

    agentos::AuditLogger audit_logger(audit_path);
    audit_logger.compact_log(isolated_workspace / "runtime" / "memory");

    std::ifstream input(audit_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto contents = buffer.str();

    Expect(contents.find("\"event\":\"scheduler_run\"") != std::string::npos,
        "audit compaction should rebuild scheduler_run events from scheduler history");
    Expect(contents.find("\"schedule_id\":\"sched-1\"") != std::string::npos,
        "rebuilt scheduler_run audit event should include schedule id");
    Expect(contents.find("\"task_id\":\"sched-1.run-1\"") != std::string::npos,
        "rebuilt scheduler_run audit event should include task id");
    Expect(contents.find("\"started_epoch_ms\":1767225600000") != std::string::npos,
        "rebuilt scheduler_run audit event should preserve 64-bit started epoch milliseconds");
    Expect(contents.find("\"completed_epoch_ms\":1767225600123") != std::string::npos,
        "rebuilt scheduler_run audit event should preserve 64-bit completed epoch milliseconds");
    Expect(contents.find("\"rescheduled\":true") != std::string::npos,
        "rebuilt scheduler_run audit event should include reschedule state");
    Expect(contents.find("\"route_target\":\"file_write\"") != std::string::npos,
        "rebuilt scheduler_run audit event should include route target");
    Expect(contents.find("\"schedule_id\":\"stale\"") == std::string::npos,
        "audit compaction should replace stale scheduler_run events with scheduler history state");
    Expect(contents.find("\"event\":\"trust\"") != std::string::npos,
        "audit compaction should preserve unrelated global audit events");
}

void TestAuditLoggerMergesMixedRecoveredTimeline(const std::filesystem::path& workspace) {
    const auto isolated_workspace = workspace / "audit_mixed_recovery_isolated";
    std::filesystem::remove_all(isolated_workspace);
    std::filesystem::create_directories(isolated_workspace / "runtime" / "memory");
    std::filesystem::create_directories(isolated_workspace / "runtime" / "scheduler");

    {
        std::ofstream tasks(isolated_workspace / "runtime" / "memory" / "task_log.tsv", std::ios::binary);
        tasks << "task-1\twrite_file\tmixed objective\t\t1\t0\t42\tskill\tfile_write\t\t\n";
    }
    {
        std::ofstream steps(isolated_workspace / "runtime" / "memory" / "step_log.tsv", std::ios::binary);
        steps << "task-1\tskill\tfile_write\t1\t17\t0\twrote file\t\t\n";
    }
    {
        std::ofstream runs(isolated_workspace / "runtime" / "scheduler" / "runs.tsv", std::ios::binary);
        runs << "sched-mixed\tsched-mixed.run-1\t1767225602500\t1767225603500\t1\t1\t0\t"
             << "skill\tfile_write\t\t\t25\n";
    }

    const auto audit_path = isolated_workspace / "runtime" / "audit.log";
    {
        std::ofstream audit(audit_path, std::ios::binary);
        audit << "{\"ts\":\"2026-01-01T00:00:00Z\",\"event\":\"trust\",\"action\":\"before\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:01Z\",\"event\":\"task_start\",\"task_id\":\"task-1\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:02Z\",\"event\":\"route\",\"task_id\":\"task-1\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:03Z\",\"event\":\"policy\",\"task_id\":\"task-1\",\"decision\":\"allow\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:04Z\",\"event\":\"step\",\"task_id\":\"task-1\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:05Z\",\"event\":\"policy\",\"task_id\":\"orphan-task\",\"decision\":\"manual\"}\n";
        audit << "{\"event\":\"config_diagnostic\",\"source\":\"untimed-global\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:06Z\",\"event\":\"task_end\",\"task_id\":\"task-1\"}\n";
        audit << "{\"ts\":\"2026-01-01T00:00:07Z\",\"event\":\"scheduler_run\",\"schedule_id\":\"stale\"}\n";
    }

    agentos::AuditLogger audit_logger(audit_path);
    audit_logger.compact_log(isolated_workspace / "runtime" / "memory");
    const auto contents = ReadFile(audit_path);

    const auto trust_index = contents.find("\"event\":\"trust\"");
    const auto task_start_index = contents.find("\"event\":\"task_start\"");
    const auto route_index = contents.find("\"event\":\"route\"");
    const auto policy_index = contents.find("\"event\":\"policy\",\"task_id\":\"task-1\"");
    const auto scheduler_index = contents.find("\"event\":\"scheduler_run\"");
    const auto step_index = contents.find("\"event\":\"step\"");
    const auto orphan_index = contents.find("\"task_id\":\"orphan-task\"");
    const auto untimed_global_index = contents.find("\"source\":\"untimed-global\"");
    const auto task_end_index = contents.find("\"event\":\"task_end\"");

    Expect(trust_index != std::string::npos, "audit mixed recovery should preserve timed global events");
    Expect(task_start_index != std::string::npos, "audit mixed recovery should rebuild task_start");
    Expect(route_index != std::string::npos, "audit mixed recovery should rebuild route");
    Expect(policy_index != std::string::npos, "audit mixed recovery should preserve task-scoped policy events");
    Expect(step_index != std::string::npos, "audit mixed recovery should rebuild step events");
    Expect(scheduler_index != std::string::npos, "audit mixed recovery should rebuild scheduler_run events");
    Expect(orphan_index != std::string::npos, "audit mixed recovery should preserve orphan task-scoped events");
    Expect(untimed_global_index != std::string::npos, "audit mixed recovery should preserve untimed global events");
    Expect(task_end_index != std::string::npos, "audit mixed recovery should rebuild task_end");
    Expect(contents.find("\"schedule_id\":\"stale\"") == std::string::npos,
        "audit mixed recovery should replace stale scheduler_run events");

    Expect(trust_index < task_start_index && task_start_index < route_index,
        "audit mixed recovery should keep global and task lifecycle ordering by timestamp");
    Expect(route_index < policy_index && policy_index < scheduler_index && scheduler_index < step_index,
        "audit mixed recovery should merge task policy, scheduler run, and step chunks by timestamp");
    Expect(step_index < orphan_index && orphan_index < task_end_index,
        "audit mixed recovery should merge orphan task-scoped events by timestamp");
    Expect(task_end_index < untimed_global_index,
        "audit mixed recovery should place untimed global events after timed chunks");
}

}  // namespace

int main() {
    const auto workspace = FreshWorkspace();

    TestAtomicFileWriteReplacesContent(workspace);
    TestAppendLineToFileAppendsAndReleasesLock(workspace);
    TestAppendLineToFileRecoversPendingAppend(workspace);
    TestAppendLineToFileDoesNotDuplicateRecoveredAppend(workspace);
    TestAppendLineToFileConcurrentWriters(workspace);
    TestAtomicFileWriteConcurrentWriters(workspace);
    TestStorageTransactionCommitsMultipleFiles(workspace);
    TestStorageTransactionRollsBackUncommittedPrepare(workspace);
    TestStorageTransactionRecoversCommittedPrepare(workspace);
    TestStorageTransactionSkipsCorruptCommittedPrepare(workspace);
    TestTsvStorageBackendCoreCapabilities(workspace);
    TestStorageVersionStorePersistsManifest(workspace);
    TestStorageVersionStoreMigratesMissingManifest(workspace);
    TestStorageVersionStoreMigratesLegacyManifest(workspace);
    TestStorageVersionStoreMigratesLegacyPaths(workspace);
    TestStoragePolicyDecisionDefaultsToTsvMvp();
    TestWorkflowStoreCompactsLegacyFormat(workspace);
    TestSchedulerCompactsLegacyTaskStore(workspace);
    TestSessionStoreCompactsCurrentFormat(workspace);
    TestExecutionCacheCompactsCurrentFormat(workspace);
    TestLessonStoreCompactsCurrentFormat(workspace);
    TestStorageExportCopiesManifestedFiles(workspace);
    TestStorageImportCopiesManifestedFiles(workspace);
    TestMemoryManagerCompactsLogs(workspace);
    TestSchedulerCompactsHistory(workspace);
    TestAuditLoggerCompactsLog(workspace);
    TestAuditLoggerRebuildsSchedulerRunEvents(workspace);
    TestAuditLoggerMergesMixedRecoveredTimeline(workspace);

    if (failures != 0) {
        std::cerr << failures << " storage test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_storage_tests passed\n";
    return 0;
}
