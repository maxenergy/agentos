#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <sstream>

namespace agentos {

std::vector<std::string> SplitNonEmpty(const std::string& value, const char delimiter) {
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

std::vector<std::string> SplitTsvFields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto delimiter = line.find('\t', start);
        if (delimiter == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, delimiter - start));
        start = delimiter + 1;
    }
    return fields;
}

bool ParseStrictInt(const std::string& value, int& output) {
    try {
        std::size_t parsed = 0;
        const auto parsed_value = std::stoi(value, &parsed);
        if (parsed != value.size()) {
            return false;
        }
        output = parsed_value;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseStrictSize(const std::string& value, std::size_t& output) {
    if (!value.empty() && value[0] == '-') {
        return false;
    }
    try {
        std::size_t parsed = 0;
        const auto parsed_value = static_cast<std::size_t>(std::stoull(value, &parsed));
        if (parsed != value.size()) {
            return false;
        }
        output = parsed_value;
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

std::string TrimWhitespace(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

}  // namespace

bool IsLikelyJsonObjectString(const std::string& value) {
    const auto trimmed = TrimWhitespace(value);
    if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}') {
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : trimmed) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth < 0) {
                return false;
            }
        }
    }

    return depth == 0 && !in_string && !escaped;
}

std::string JoinStrings(const std::vector<std::string>& values, const char delimiter) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << delimiter;
        }
        output << values[index];
    }
    return output.str();
}

}  // namespace agentos
