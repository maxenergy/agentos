#include "core/execution/execution_cache.hpp"

#include <fstream>
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

    auto result = it->second;
    result.from_cache = true;
    return result;
}

void ExecutionCache::store(const std::string& idempotency_key, const TaskRunResult& result) {
    if (idempotency_key.empty() || !result.success) {
        return;
    }

    auto cached = result;
    cached.from_cache = false;
    cached.steps.clear();
    entries_[idempotency_key] = std::move(cached);
    flush();
}

const std::filesystem::path& ExecutionCache::cache_path() const {
    return cache_path_;
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

        TaskRunResult result;
        result.success = parts[1] == "1";
        result.route_kind = ParseRouteTargetKind(parts[2]);
        result.route_target = parts[3];
        result.duration_ms = std::stoi(parts[4]);
        result.summary = parts[5];
        result.output_json = parts[6];
        result.error_code = parts[7];
        result.error_message = parts[8];
        entries_[parts[0]] = std::move(result);
    }
}

void ExecutionCache::flush() const {
    if (cache_path_.empty()) {
        return;
    }

    if (!cache_path_.parent_path().empty()) {
        std::filesystem::create_directories(cache_path_.parent_path());
    }

    std::ofstream output(cache_path_, std::ios::binary | std::ios::trunc);
    for (const auto& [key, result] : entries_) {
        output
            << EncodeField(key) << kDelimiter
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
}

}  // namespace agentos
