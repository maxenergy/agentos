#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace agentos {

struct StorageTransactionWrite {
    std::filesystem::path target_path;
    std::string content;
};

struct StorageTransactionRecoveryResult {
    int committed_replayed = 0;
    int rolled_back = 0;
    int failed = 0;
    int files_applied = 0;
};

void PrepareStorageTransaction(
    const std::filesystem::path& runtime_dir,
    std::string_view transaction_id,
    const std::vector<StorageTransactionWrite>& writes);

void MarkStorageTransactionCommitted(
    const std::filesystem::path& runtime_dir,
    std::string_view transaction_id);

int CommitPreparedStorageTransaction(
    const std::filesystem::path& runtime_dir,
    std::string_view transaction_id);

int WriteFilesTransactionally(
    const std::filesystem::path& runtime_dir,
    std::string_view transaction_id,
    const std::vector<StorageTransactionWrite>& writes);

StorageTransactionRecoveryResult RecoverStorageTransactions(const std::filesystem::path& runtime_dir);

}  // namespace agentos
