#include "memory/workflow_store.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';
constexpr char kStepDelimiter = ',';

std::string EncodePercentField(const std::string& value, const bool encode_step_delimiter = false) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r' ||
            (encode_step_delimiter && ch == kStepDelimiter)) {
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

std::string DecodePercentField(const std::string& value) {
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
            parts.push_back(DecodePercentField(line.substr(start)));
            break;
        }

        parts.push_back(DecodePercentField(line.substr(start, delimiter - start)));
        start = delimiter + 1;
    }

    return parts;
}

std::string SerializeList(const std::vector<std::string>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << kStepDelimiter;
        }
        output << EncodePercentField(values[index], true);
    }
    return output.str();
}

std::vector<std::string> ParseList(const std::string& value) {
    std::vector<std::string> values;
    std::size_t start = 0;

    while (start <= value.size()) {
        const auto delimiter = value.find(kStepDelimiter, start);
        const auto item = delimiter == std::string::npos
            ? value.substr(start)
            : value.substr(start, delimiter - start);
        if (!item.empty()) {
            values.push_back(DecodePercentField(item));
        }

        if (delimiter == std::string::npos) {
            break;
        }
        start = delimiter + 1;
    }

    return values;
}

int ParseInt(const std::string& value, const int fallback = 0) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

double ParseDouble(const std::string& value, const double fallback = 0.0) {
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
}

}  // namespace

WorkflowStore::WorkflowStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    if (!store_path_.parent_path().empty()) {
        std::filesystem::create_directories(store_path_.parent_path());
    }
    load();
}

WorkflowDefinition WorkflowStore::save(WorkflowDefinition workflow) {
    remove(workflow.name);
    workflows_.push_back(std::move(workflow));
    flush();
    return workflows_.back();
}

bool WorkflowStore::remove(const std::string& name) {
    const auto previous_size = workflows_.size();
    workflows_.erase(std::remove_if(workflows_.begin(), workflows_.end(), [&](const WorkflowDefinition& workflow) {
        return workflow.name == name;
    }), workflows_.end());

    const bool removed = previous_size != workflows_.size();
    if (removed) {
        flush();
    }
    return removed;
}

std::optional<WorkflowDefinition> WorkflowStore::find(const std::string& name) const {
    const auto it = std::find_if(workflows_.begin(), workflows_.end(), [&](const WorkflowDefinition& workflow) {
        return workflow.name == name;
    });
    if (it == workflows_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<WorkflowDefinition> WorkflowStore::list() const {
    return workflows_;
}

const std::filesystem::path& WorkflowStore::store_path() const {
    return store_path_;
}

void WorkflowStore::compact() const {
    flush();
}

WorkflowDefinition WorkflowStore::FromCandidate(const WorkflowCandidate& candidate) {
    return {
        .name = candidate.name,
        .trigger_task_type = candidate.trigger_task_type,
        .ordered_steps = candidate.ordered_steps,
        .required_inputs = {},
        .input_equals = {},
        .input_number_gte = {},
        .input_number_lte = {},
        .input_bool = {},
        .input_regex = {},
        .input_any = {},
        .input_expr = {},
        .source = "candidate",
        .enabled = false,
        .use_count = candidate.use_count,
        .success_count = candidate.success_count,
        .failure_count = candidate.failure_count,
        .success_rate = candidate.success_rate,
        .avg_duration_ms = candidate.avg_duration_ms,
        .score = candidate.score,
    };
}

void WorkflowStore::load() {
    workflows_.clear();
    if (store_path_.empty()) {
        return;
    }

    std::ifstream input(store_path_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 11) {
            continue;
        }

        workflows_.push_back(WorkflowDefinition{
            .name = parts[0],
            .trigger_task_type = parts[2],
            .ordered_steps = ParseList(parts[3]),
            .required_inputs = parts.size() >= 12 ? ParseList(parts[11]) : std::vector<std::string>{},
            .input_equals = parts.size() >= 13 ? ParseList(parts[12]) : std::vector<std::string>{},
            .input_number_gte = parts.size() >= 14 ? ParseList(parts[13]) : std::vector<std::string>{},
            .input_number_lte = parts.size() >= 15 ? ParseList(parts[14]) : std::vector<std::string>{},
            .input_bool = parts.size() >= 16 ? ParseList(parts[15]) : std::vector<std::string>{},
            .input_regex = parts.size() >= 17 ? ParseList(parts[16]) : std::vector<std::string>{},
            .input_any = parts.size() >= 18 ? ParseList(parts[17]) : std::vector<std::string>{},
            .input_expr = parts.size() >= 19 ? ParseList(parts[18]) : std::vector<std::string>{},
            .source = parts[4],
            .enabled = parts[1] == "1",
            .use_count = ParseInt(parts[5]),
            .success_count = ParseInt(parts[6]),
            .failure_count = ParseInt(parts[7]),
            .success_rate = ParseDouble(parts[8]),
            .avg_duration_ms = ParseDouble(parts[9]),
            .score = ParseDouble(parts[10]),
        });
    }
}

void WorkflowStore::flush() const {
    if (store_path_.empty()) {
        return;
    }

    std::ostringstream output;
    for (const auto& workflow : workflows_) {
        output
            << EncodePercentField(workflow.name) << kDelimiter
            << (workflow.enabled ? "1" : "0") << kDelimiter
            << EncodePercentField(workflow.trigger_task_type) << kDelimiter
            << EncodePercentField(SerializeList(workflow.ordered_steps)) << kDelimiter
            << EncodePercentField(workflow.source) << kDelimiter
            << workflow.use_count << kDelimiter
            << workflow.success_count << kDelimiter
            << workflow.failure_count << kDelimiter
            << workflow.success_rate << kDelimiter
            << workflow.avg_duration_ms << kDelimiter
            << workflow.score << kDelimiter
            << EncodePercentField(SerializeList(workflow.required_inputs)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_equals)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_number_gte)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_number_lte)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_bool)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_regex)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_any)) << kDelimiter
            << EncodePercentField(SerializeList(workflow.input_expr))
            << '\n';
    }

    WriteFileAtomically(store_path_, output.str());
}

}  // namespace agentos
