#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

struct LessonRecord {
    std::string lesson_id;
    std::string task_type;
    std::string target_name;
    std::string error_code;
    std::string summary;
    int occurrence_count = 0;
    std::string last_task_id;
    bool enabled = true;
};

class LessonStore {
public:
    LessonStore() = default;
    explicit LessonStore(std::filesystem::path store_path);

    LessonRecord save(LessonRecord lesson);
    bool remove(const std::string& lesson_id);
    [[nodiscard]] std::optional<LessonRecord> find(const std::string& lesson_id) const;
    [[nodiscard]] std::vector<LessonRecord> list() const;
    [[nodiscard]] const std::filesystem::path& store_path() const;

    std::optional<LessonRecord> record_failure(const TaskRequest& task, const TaskRunResult& result);
    void compact() const;

private:
    static std::string BuildLessonId(const TaskRequest& task, const TaskRunResult& result);
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<LessonRecord> lessons_;
};

}  // namespace agentos
