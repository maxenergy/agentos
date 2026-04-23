#include "hosts/cli/cli_spec_loader.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>

namespace agentos {

namespace {

constexpr char kFieldDelimiter = '\t';
constexpr char kListDelimiter = ',';

std::vector<std::string> Split(const std::string& value, const char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        if (!part.empty()) {
            parts.push_back(std::move(part));
        }
    }
    return parts;
}

int ParseInt(const std::string& value, const int fallback) {
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

std::size_t ParseSize(const std::string& value, const std::size_t fallback) {
    try {
        return static_cast<std::size_t>(std::stoull(value));
    } catch (...) {
        return fallback;
    }
}

std::vector<std::string> SplitFields(const std::string& line) {
    return Split(line, kFieldDelimiter);
}

bool IsCommentOrEmpty(const std::string& line) {
    return line.empty() || line[0] == '#';
}

std::optional<CliSpec> ParseCliSpecLine(const std::string& line) {
    const auto fields = SplitFields(line);
    if (fields.size() < 9) {
        return std::nullopt;
    }

    CliSpec spec{
        .name = fields[0],
        .description = fields[1],
        .binary = fields[2],
        .args_template = Split(fields[3], kListDelimiter),
        .required_args = Split(fields[4], kListDelimiter),
        .input_schema_json = fields.size() >= 10 ? fields[9] : R"({"type":"object"})",
        .output_schema_json = fields.size() >= 11 ? fields[10] : R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = fields[5],
        .risk_level = fields[6],
        .permissions = Split(fields[7], kListDelimiter),
        .timeout_ms = ParseInt(fields[8], 3000),
        .output_limit_bytes = fields.size() >= 12 ? ParseSize(fields[11], 1024 * 1024) : 1024 * 1024,
        .env_allowlist = fields.size() >= 13 ? Split(fields[12], kListDelimiter) : std::vector<std::string>{},
    };

    if (spec.name.empty() || spec.binary.empty()) {
        return std::nullopt;
    }
    return spec;
}

}  // namespace

std::vector<CliSpec> LoadCliSpecsFromDirectory(const std::filesystem::path& spec_dir) {
    std::vector<CliSpec> specs;
    if (spec_dir.empty() || !std::filesystem::exists(spec_dir)) {
        return specs;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(spec_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".tsv") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            if (IsCommentOrEmpty(line)) {
                continue;
            }
            if (const auto spec = ParseCliSpecLine(line); spec.has_value()) {
                specs.push_back(*spec);
            }
        }
    }

    return specs;
}

}  // namespace agentos
