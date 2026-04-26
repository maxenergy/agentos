#include "utils/atomic_file.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace agentos {

namespace {

constexpr auto kLockTimeout = std::chrono::seconds(2);
constexpr auto kLockRetryDelay = std::chrono::milliseconds(20);

std::filesystem::path MakeTempPath(const std::filesystem::path& path) {
    static std::atomic<unsigned long long> counter{0};
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "." + std::to_string(counter.fetch_add(1));
    return path.parent_path() / (path.filename().string() + ".tmp." + suffix);
}

std::filesystem::path MakeLockPath(const std::filesystem::path& path) {
    return path.parent_path() / (path.filename().string() + ".lock");
}

std::filesystem::path MakeAppendRecoveryPath(const std::filesystem::path& path) {
    static std::atomic<unsigned long long> counter{0};
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "." + std::to_string(counter.fetch_add(1));
    return path.parent_path() / (path.filename().string() + ".append.tmp." + suffix);
}

bool IsAppendRecoveryPath(const std::filesystem::path& target_path, const std::filesystem::path& candidate_path) {
    const auto candidate_name = candidate_path.filename().string();
    const auto prefix = target_path.filename().string() + ".append.tmp.";
    return candidate_name.rfind(prefix, 0) == 0;
}

std::uintmax_t ExistingFileSize(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

bool ReadAppendRecoveryIntent(
    const std::filesystem::path& recovery_path,
    std::uintmax_t& expected_size,
    std::string& line) {
    std::ifstream input(recovery_path, std::ios::binary);
    if (!input) {
        return false;
    }

    std::string size_line;
    if (!std::getline(input, size_line)) {
        return false;
    }
    if (!size_line.empty() && size_line.back() == '\r') {
        size_line.pop_back();
    }

    try {
        expected_size = static_cast<std::uintmax_t>(std::stoull(size_line));
    } catch (...) {
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    line = buffer.str();
    return true;
}

void WriteAppendRecoveryIntent(
    const std::filesystem::path& recovery_path,
    const std::uintmax_t expected_size,
    const std::string_view line) {
    std::ofstream output(recovery_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("could not open append recovery file: " + recovery_path.string());
    }
    output << expected_size << '\n';
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("could not write append recovery file: " + recovery_path.string());
    }
}

bool TargetContainsRecoveredLineAtOffset(
    const std::filesystem::path& target_path,
    const std::uintmax_t expected_size,
    const std::string& line) {
    std::ifstream input(target_path, std::ios::binary);
    if (!input) {
        return false;
    }

    input.seekg(static_cast<std::streamoff>(expected_size), std::ios::beg);
    if (!input) {
        return false;
    }

    std::string existing(line.size() + 1, '\0');
    input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    existing.resize(static_cast<std::size_t>(input.gcount()));
    return existing == line + "\n";
}

void AppendRecoveredLine(const std::filesystem::path& path, const std::string& line) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        throw std::runtime_error("could not open file for append recovery: " + path.string());
    }
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    output.flush();
    if (!output) {
        throw std::runtime_error("could not recover append file: " + path.string());
    }
}

void RecoverPendingAppendsLocked(const std::filesystem::path& path) {
    if (path.parent_path().empty() || !std::filesystem::exists(path.parent_path())) {
        return;
    }

    std::vector<std::filesystem::path> recovery_paths;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.is_regular_file() && IsAppendRecoveryPath(path, entry.path())) {
            recovery_paths.push_back(entry.path());
        }
    }
    std::sort(recovery_paths.begin(), recovery_paths.end());

    for (const auto& recovery_path : recovery_paths) {
        std::uintmax_t expected_size = 0;
        std::string line;
        if (!ReadAppendRecoveryIntent(recovery_path, expected_size, line)) {
            std::filesystem::remove(recovery_path);
            continue;
        }

        const auto current_size = ExistingFileSize(path);
        if (current_size == expected_size) {
            AppendRecoveredLine(path, line);
        } else if (current_size > expected_size && !TargetContainsRecoveredLineAtOffset(path, expected_size, line)) {
            throw std::runtime_error("append recovery conflict for file: " + path.string());
        }
        std::filesystem::remove(recovery_path);
    }
}

class AtomicFileLock {
public:
    explicit AtomicFileLock(std::filesystem::path lock_path)
        : lock_path_(std::move(lock_path)) {
        acquire();
    }

    AtomicFileLock(const AtomicFileLock&) = delete;
    AtomicFileLock& operator=(const AtomicFileLock&) = delete;

    ~AtomicFileLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            close(fd_);
            std::filesystem::remove(lock_path_);
        }
#endif
    }

private:
    void acquire() {
        const auto deadline = std::chrono::steady_clock::now() + kLockTimeout;
        while (true) {
#ifdef _WIN32
            handle_ = CreateFileW(
                lock_path_.wstring().c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return;
            }

            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                throw std::runtime_error("could not create atomic write lock with Windows error " + std::to_string(error));
            }
#else
            fd_ = open(lock_path_.string().c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd_ >= 0) {
                return;
            }

            if (errno != EEXIST) {
                throw std::runtime_error("could not create atomic write lock: " + std::string(std::strerror(errno)));
            }
#endif

            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("timed out waiting for atomic write lock: " + lock_path_.string());
            }
            std::this_thread::sleep_for(kLockRetryDelay);
        }
    }

    std::filesystem::path lock_path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

void ReplaceFile(const std::filesystem::path& temp_path, const std::filesystem::path& target_path) {
#ifdef _WIN32
    if (MoveFileExW(
            temp_path.wstring().c_str(),
            target_path.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const auto error = GetLastError();
        std::filesystem::remove(temp_path);
        throw std::runtime_error("atomic file replace failed with Windows error " + std::to_string(error));
    }
#else
    std::error_code error;
    std::filesystem::rename(temp_path, target_path, error);
    if (error) {
        std::filesystem::remove(temp_path);
        throw std::runtime_error("atomic file replace failed: " + error.message());
    }
#endif
}

}  // namespace

void AppendLineToFile(const std::filesystem::path& path, const std::string_view line) {
    if (path.empty()) {
        return;
    }

    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }

    AtomicFileLock lock(MakeLockPath(path));
    RecoverPendingAppendsLocked(path);
    const auto recovery_path = MakeAppendRecoveryPath(path);
    WriteAppendRecoveryIntent(recovery_path, ExistingFileSize(path), line);

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        std::filesystem::remove(recovery_path);
        throw std::runtime_error("could not open file for append: " + path.string());
    }
    output.write(line.data(), static_cast<std::streamsize>(line.size()));
    output.put('\n');
    output.flush();
    if (!output) {
        std::filesystem::remove(recovery_path);
        throw std::runtime_error("could not append file: " + path.string());
    }
    output.close();
    std::filesystem::remove(recovery_path);
}

void WriteFileAtomically(const std::filesystem::path& path, const std::string_view content) {
    if (path.empty()) {
        return;
    }

    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }

    AtomicFileLock lock(MakeLockPath(path));
    const auto temp_path = MakeTempPath(path);
    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not open temporary file for atomic write: " + temp_path.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temp_path);
            throw std::runtime_error("could not write temporary file for atomic replace: " + temp_path.string());
        }
    }

    ReplaceFile(temp_path, path);
}

}  // namespace agentos
