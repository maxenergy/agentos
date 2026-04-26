#include "storage/storage_transaction.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::filesystem::path TransactionsDir(const std::filesystem::path& runtime_dir) {
    return runtime_dir / ".transactions";
}

std::filesystem::path TransactionDir(const std::filesystem::path& runtime_dir, const std::string_view transaction_id) {
    if (transaction_id.empty()) {
        throw std::runtime_error("storage transaction id is required");
    }
    const std::string id(transaction_id);
    if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos || id.find("..") != std::string::npos) {
        throw std::runtime_error("storage transaction id must be a simple name");
    }
    return TransactionsDir(runtime_dir) / id;
}

std::filesystem::path PreparePath(const std::filesystem::path& transaction_dir) {
    return transaction_dir / "prepare.tsv";
}

std::filesystem::path CommitPath(const std::filesystem::path& transaction_dir) {
    return transaction_dir / "commit";
}

std::string EscapeField(const std::string& value) {
    std::ostringstream output;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                output << "\\\\";
                break;
            case '\t':
                output << "\\t";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            default:
                output << ch;
                break;
        }
    }
    return output.str();
}

std::string UnescapeField(const std::string& value) {
    std::string output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1 >= value.size()) {
            output.push_back(value[index]);
            continue;
        }
        const char escaped = value[++index];
        switch (escaped) {
            case 't':
                output.push_back('\t');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case '\\':
                output.push_back('\\');
                break;
            default:
                output.push_back(escaped);
                break;
        }
    }
    return output;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read storage transaction file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

struct PreparedWrite {
    std::filesystem::path target_path;
    std::filesystem::path staged_path;
};

std::vector<PreparedWrite> LoadPreparedWrites(const std::filesystem::path& transaction_dir) {
    std::ifstream input(PreparePath(transaction_dir), std::ios::binary);
    if (!input) {
        throw std::runtime_error("storage transaction prepare marker is missing: " + transaction_dir.string());
    }

    std::vector<PreparedWrite> writes;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto delimiter = line.find('\t');
        if (delimiter == std::string::npos) {
            throw std::runtime_error("invalid storage transaction prepare row: " + transaction_dir.string());
        }
        writes.push_back(PreparedWrite{
            .target_path = std::filesystem::path(UnescapeField(line.substr(0, delimiter))),
            .staged_path = transaction_dir / UnescapeField(line.substr(delimiter + 1)),
        });
    }
    return writes;
}

int ApplyPreparedTransaction(const std::filesystem::path& transaction_dir) {
    struct PendingApply {
        std::filesystem::path target_path;
        std::string content;
    };

    std::vector<PendingApply> pending;
    int applied = 0;
    for (const auto& write : LoadPreparedWrites(transaction_dir)) {
        pending.push_back(PendingApply{
            .target_path = write.target_path,
            .content = ReadFile(write.staged_path),
        });
    }

    for (const auto& write : pending) {
        WriteFileAtomically(write.target_path, write.content);
        applied += 1;
    }
    return applied;
}

}  // namespace

void PrepareStorageTransaction(
    const std::filesystem::path& runtime_dir,
    const std::string_view transaction_id,
    const std::vector<StorageTransactionWrite>& writes) {
    if (runtime_dir.empty()) {
        throw std::runtime_error("storage transaction runtime dir is required");
    }
    if (writes.empty()) {
        throw std::runtime_error("storage transaction requires at least one write");
    }

    const auto transaction_dir = TransactionDir(runtime_dir, transaction_id);
    std::filesystem::remove_all(transaction_dir);
    std::filesystem::create_directories(transaction_dir);

    std::ostringstream manifest;
    for (std::size_t index = 0; index < writes.size(); ++index) {
        if (writes[index].target_path.empty()) {
            throw std::runtime_error("storage transaction target path is required");
        }
        const auto staged_name = "file_" + std::to_string(index) + ".data";
        WriteFileAtomically(transaction_dir / staged_name, writes[index].content);
        manifest << EscapeField(writes[index].target_path.string()) << '\t' << staged_name << '\n';
    }
    WriteFileAtomically(PreparePath(transaction_dir), manifest.str());
}

void MarkStorageTransactionCommitted(
    const std::filesystem::path& runtime_dir,
    const std::string_view transaction_id) {
    const auto transaction_dir = TransactionDir(runtime_dir, transaction_id);
    if (!std::filesystem::exists(PreparePath(transaction_dir))) {
        throw std::runtime_error("cannot commit unprepared storage transaction: " + transaction_dir.string());
    }
    WriteFileAtomically(CommitPath(transaction_dir), "committed\n");
}

int CommitPreparedStorageTransaction(
    const std::filesystem::path& runtime_dir,
    const std::string_view transaction_id) {
    const auto transaction_dir = TransactionDir(runtime_dir, transaction_id);
    MarkStorageTransactionCommitted(runtime_dir, transaction_id);
    const int applied = ApplyPreparedTransaction(transaction_dir);
    std::filesystem::remove_all(transaction_dir);
    return applied;
}

int WriteFilesTransactionally(
    const std::filesystem::path& runtime_dir,
    const std::string_view transaction_id,
    const std::vector<StorageTransactionWrite>& writes) {
    PrepareStorageTransaction(runtime_dir, transaction_id, writes);
    return CommitPreparedStorageTransaction(runtime_dir, transaction_id);
}

StorageTransactionRecoveryResult RecoverStorageTransactions(const std::filesystem::path& runtime_dir) {
    StorageTransactionRecoveryResult result;
    const auto transactions_dir = TransactionsDir(runtime_dir);
    if (!std::filesystem::exists(transactions_dir)) {
        return result;
    }

    std::vector<std::filesystem::path> transaction_dirs;
    for (const auto& entry : std::filesystem::directory_iterator(transactions_dir)) {
        if (entry.is_directory()) {
            transaction_dirs.push_back(entry.path());
        }
    }
    std::sort(transaction_dirs.begin(), transaction_dirs.end());

    for (const auto& transaction_dir : transaction_dirs) {
        if (std::filesystem::exists(CommitPath(transaction_dir))) {
            try {
                result.files_applied += ApplyPreparedTransaction(transaction_dir);
                result.committed_replayed += 1;
            } catch (const std::exception&) {
                result.failed += 1;
            }
        } else {
            result.rolled_back += 1;
        }
        std::filesystem::remove_all(transaction_dir);
    }
    return result;
}

}  // namespace agentos
