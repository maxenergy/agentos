#include "core/execution/execution_cache.hpp"

#include "utils/atomic_file.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <vector>

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

RouteTargetKind ParseRouteTargetKind(const std::string& value) {
    if (value == "skill") {
        return RouteTargetKind::skill;
    }
    if (value == "agent") {
        return RouteTargetKind::agent;
    }
    return RouteTargetKind::none;
}

void AppendFingerprintField(std::ostringstream& output, const std::string& key, const std::string& value) {
    output << EncodeField(key) << '=' << EncodeField(value) << '\n';
}

}  // namespace

ExecutionCache::ExecutionCache(std::filesystem::path cache_path)
    : cache_path_(std::move(cache_path)) {
    load();
}

std::optional<TaskRunResult> ExecutionCache::find(const std::string& idempotency_key) const {
    if (idempotency_key.empty()) {
        return std::nullopt;
    }

    const auto it = entries_.find(idempotency_key);
    if (it == entries_.end()) {
        return std::nullopt;
    }

    auto result = it->second.result;
    result.from_cache = true;
    return result;
}

std::optional<TaskRunResult> ExecutionCache::find(
    const std::string& idempotency_key,
    const std::string& input_fingerprint) const {
    if (idempotency_key.empty() || input_fingerprint.empty()) {
        return std::nullopt;
    }

    const auto it = entries_.find(idempotency_key);
    if (it == entries_.end() || it->second.input_fingerprint != input_fingerprint) {
        return std::nullopt;
    }

    auto result = it->second.result;
    result.from_cache = true;
    return result;
}

void ExecutionCache::store(const std::string& idempotency_key, const TaskRunResult& result) {
    store(idempotency_key, "", result);
}

void ExecutionCache::store(
    const std::string& idempotency_key,
    const std::string& input_fingerprint,
    const TaskRunResult& result) {
    if (idempotency_key.empty() || !result.success) {
        return;
    }

    auto cached = result;
    cached.from_cache = false;
    cached.steps.clear();
    entries_[idempotency_key] = Entry{
        .input_fingerprint = input_fingerprint,
        .result = std::move(cached),
    };
    flush();
}

void ExecutionCache::compact() const {
    flush();
}

const std::filesystem::path& ExecutionCache::cache_path() const {
    return cache_path_;
}

std::string ExecutionCache::fingerprint_for_task(const TaskRequest& task) {
    std::ostringstream output;
    AppendFingerprintField(output, "task_type", task.task_type);
    AppendFingerprintField(output, "objective", task.objective);
    AppendFingerprintField(output, "workspace_path", task.workspace_path.string());
    AppendFingerprintField(output, "user_id", task.user_id);
    AppendFingerprintField(output, "preferred_target", task.preferred_target.value_or(""));
    AppendFingerprintField(output, "remote_trigger", task.remote_trigger ? "1" : "0");
    AppendFingerprintField(output, "origin_identity_id", task.origin_identity_id);
    AppendFingerprintField(output, "origin_device_id", task.origin_device_id);
    AppendFingerprintField(output, "timeout_ms", std::to_string(task.timeout_ms));
    AppendFingerprintField(output, "budget_limit", std::to_string(task.budget_limit));
    AppendFingerprintField(output, "allow_high_risk", task.allow_high_risk ? "1" : "0");
    AppendFingerprintField(output, "allow_network", task.allow_network ? "1" : "0");
    AppendFingerprintField(output, "approval_id", task.approval_id);

    std::map<std::string, std::string> sorted_inputs(task.inputs.begin(), task.inputs.end());
    for (const auto& [key, value] : sorted_inputs) {
        AppendFingerprintField(output, "input:" + key, value);
    }
    std::map<std::string, bool> grants;
    for (const auto& grant : task.permission_grants) {
        grants[grant] = true;
    }
    for (const auto& [grant, unused] : grants) {
        (void)unused;
        AppendFingerprintField(output, "grant", grant);
    }
    return output.str();
}

void ExecutionCache::load() {
    entries_.clear();
    if (cache_path_.empty()) {
        return;
    }

    std::ifstream input(cache_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 9) {
            continue;
        }

        const bool has_fingerprint = parts.size() >= 10;
        const std::size_t offset = has_fingerprint ? 1 : 0;
        TaskRunResult result;
        result.success = parts[1 + offset] == "1";
        result.route_kind = ParseRouteTargetKind(parts[2 + offset]);
        result.route_target = parts[3 + offset];
        result.duration_ms = std::stoi(parts[4 + offset]);
        result.summary = parts[5 + offset];
        result.output_json = parts[6 + offset];
        result.error_code = parts[7 + offset];
        result.error_message = parts[8 + offset];
        entries_[parts[0]] = Entry{
            .input_fingerprint = has_fingerprint ? parts[1] : "",
            .result = std::move(result),
        };
    }
}

void ExecutionCache::flush() const {
    if (cache_path_.empty()) {
        return;
    }

    std::ostringstream output;
    for (const auto& [key, entry] : entries_) {
        const auto& result = entry.result;
        output
            << EncodeField(key) << kDelimiter
            << EncodeField(entry.input_fingerprint) << kDelimiter
            << (result.success ? "1" : "0") << kDelimiter
            << EncodeField(route_target_kind_name(result.route_kind)) << kDelimiter
            << EncodeField(result.route_target) << kDelimiter
            << result.duration_ms << kDelimiter
            << EncodeField(result.summary) << kDelimiter
            << EncodeField(result.output_json) << kDelimiter
            << EncodeField(result.error_code) << kDelimiter
            << EncodeField(result.error_message)
            << '\n';
    }

    WriteFileAtomically(cache_path_, output.str());
}

}  // namespace agentos
