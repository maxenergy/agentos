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
    void store(const std::string& idempotency_key, const TaskRunResult& result);

    [[nodiscard]] const std::filesystem::path& cache_path() const;

private:
    void load();
    void flush() const;

    std::filesystem::path cache_path_;
    std::unordered_map<std::string, TaskRunResult> entries_;
};

}  // namespace agentos

