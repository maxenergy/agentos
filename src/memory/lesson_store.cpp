#include "memory/lesson_store.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
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

int ParseInt(const std::string& value, const int fallback = 0) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::string NonEmptyOr(const std::string& value, const std::string& fallback) {
    return value.empty() ? fallback : value;
}

std::string StripLessonHint(const std::string& value) {
    const auto hint_position = value.find(" lesson_hint=");
    if (hint_position == std::string::npos) {
        return value;
    }
    return value.substr(0, hint_position);
}

}  // namespace

LessonStore::LessonStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    if (!store_path_.parent_path().empty()) {
        std::filesystem::create_directories(store_path_.parent_path());
    }
    load();
}

LessonRecord LessonStore::save(LessonRecord lesson) {
    remove(lesson.lesson_id);
    lessons_.push_back(std::move(lesson));
    flush();
    return lessons_.back();
}

bool LessonStore::remove(const std::string& lesson_id) {
    const auto previous_size = lessons_.size();
    lessons_.erase(std::remove_if(lessons_.begin(), lessons_.end(), [&](const LessonRecord& lesson) {
        return lesson.lesson_id == lesson_id;
    }), lessons_.end());

    const bool removed = previous_size != lessons_.size();
    if (removed) {
        flush();
    }
    return removed;
}

std::optional<LessonRecord> LessonStore::find(const std::string& lesson_id) const {
    const auto it = std::find_if(lessons_.begin(), lessons_.end(), [&](const LessonRecord& lesson) {
        return lesson.lesson_id == lesson_id;
    });
    if (it == lessons_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<LessonRecord> LessonStore::list() const {
    return lessons_;
}

const std::filesystem::path& LessonStore::store_path() const {
    return store_path_;
}

std::optional<LessonRecord> LessonStore::record_failure(const TaskRequest& task, const TaskRunResult& result) {
    if (result.success || result.from_cache) {
        return std::nullopt;
    }

    const auto lesson_id = BuildLessonId(task, result);
    auto lesson = find(lesson_id).value_or(LessonRecord{
        .lesson_id = lesson_id,
        .task_type = task.task_type,
        .target_name = NonEmptyOr(result.route_target, route_target_kind_name(result.route_kind)),
        .error_code = NonEmptyOr(result.error_code, "UnknownFailure"),
        .summary = StripLessonHint(NonEmptyOr(result.error_message, result.summary)),
        .occurrence_count = 0,
        .last_task_id = task.task_id,
        .enabled = true,
    });

    lesson.occurrence_count += 1;
    lesson.last_task_id = task.task_id;
    lesson.summary = StripLessonHint(NonEmptyOr(result.error_message, result.summary));
    return save(std::move(lesson));
}

void LessonStore::compact() const {
    flush();
}

std::string LessonStore::BuildLessonId(const TaskRequest& task, const TaskRunResult& result) {
    return task.task_type + "|" +
           NonEmptyOr(result.route_target, route_target_kind_name(result.route_kind)) + "|" +
           NonEmptyOr(result.error_code, "UnknownFailure");
}

void LessonStore::load() {
    lessons_.clear();
    if (store_path_.empty()) {
        return;
    }

    std::ifstream input(store_path_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 8) {
            continue;
        }

        lessons_.push_back(LessonRecord{
            .lesson_id = parts[0],
            .task_type = parts[2],
            .target_name = parts[3],
            .error_code = parts[4],
            .summary = parts[7],
            .occurrence_count = ParseInt(parts[5]),
            .last_task_id = parts[6],
            .enabled = parts[1] == "1",
        });
    }
}

void LessonStore::flush() const {
    if (store_path_.empty()) {
        return;
    }

    std::ostringstream output;
    for (const auto& lesson : lessons_) {
        output
            << EncodeField(lesson.lesson_id) << kDelimiter
            << (lesson.enabled ? "1" : "0") << kDelimiter
            << EncodeField(lesson.task_type) << kDelimiter
            << EncodeField(lesson.target_name) << kDelimiter
            << EncodeField(lesson.error_code) << kDelimiter
            << lesson.occurrence_count << kDelimiter
            << EncodeField(lesson.last_task_id) << kDelimiter
            << EncodeField(lesson.summary)
            << '\n';
    }

    WriteFileAtomically(store_path_, output.str());
}

}  // namespace agentos
