#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace agentos {

class ExecutionCache {
public:
    ExecutionCache() = default;
    explicit ExecutionCache(std::filesystem::path cache_path);

    std::optional<TaskRunResult> find(const std::string& idempotency_key) const;
    std::optional<TaskRunResult> find(const std::string& idempotency_key, const std::string& input_fingerprint) const;
    void store(const std::string& idempotency_key, const TaskRunResult& result);
    void store(const std::string& idempotency_key, const std::string& input_fingerprint, const TaskRunResult& result);
    void compact() const;

    [[nodiscard]] const std::filesystem::path& cache_path() const;
    static std::string fingerprint_for_task(const TaskRequest& task);

private:
    struct Entry {
        std::string input_fingerprint;
        TaskRunResult result;
    };

    void load();
    void flush() const;

    std::filesystem::path cache_path_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace agentos
