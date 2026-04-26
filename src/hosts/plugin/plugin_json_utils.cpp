#include "hosts/plugin/plugin_json_utils.hpp"

#include "utils/spec_parsing.hpp"

#include <cctype>
#include <utility>

namespace agentos {

void SkipJsonWhitespace(const std::string& text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
}

std::optional<std::size_t> FindJsonValueStart(const std::string& text, const std::string& key) {
    int object_depth = 0;
    int array_depth = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char ch = text[pos];
        if (ch == '{') {
            ++object_depth;
            ++pos;
            continue;
        }
        if (ch == '}') {
            --object_depth;
            ++pos;
            continue;
        }
        if (ch == '[') {
            ++array_depth;
            ++pos;
            continue;
        }
        if (ch == ']') {
            --array_depth;
            ++pos;
            continue;
        }
        if (ch != '"') {
            ++pos;
            continue;
        }

        const auto key_start = pos;
        std::string parsed_key;
        if (!ParseJsonStringAt(text, pos, parsed_key)) {
            return std::nullopt;
        }
        auto after_key = pos;
        SkipJsonWhitespace(text, after_key);
        if (object_depth == 1 && array_depth == 0 && after_key < text.size() && text[after_key] == ':') {
            ++after_key;
            SkipJsonWhitespace(text, after_key);
            if (parsed_key == key) {
                return after_key;
            }
            pos = after_key;
            continue;
        }
        if (pos == key_start) {
            ++pos;
        }
    }
    return std::nullopt;
}

bool ParseJsonStringAt(const std::string& text, std::size_t& pos, std::string& output) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    output.clear();
    while (pos < text.size()) {
        const char ch = text[pos++];
        if (ch == '"') {
            return true;
        }
        if (ch != '\\') {
            output.push_back(ch);
            continue;
        }
        if (pos >= text.size()) {
            return false;
        }
        const char escaped = text[pos++];
        switch (escaped) {
            case '"':
            case '\\':
            case '/':
                output.push_back(escaped);
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            default:
                return false;
        }
    }
    return false;
}

std::optional<std::string> JsonStringField(const std::string& text, const std::string& key) {
    auto pos = FindJsonValueStart(text, key);
    if (!pos.has_value()) {
        return std::nullopt;
    }
    std::string output;
    if (!ParseJsonStringAt(text, *pos, output)) {
        return std::nullopt;
    }
    return output;
}

std::optional<std::vector<std::string>> JsonStringArrayField(const std::string& text, const std::string& key) {
    auto pos = FindJsonValueStart(text, key);
    if (!pos.has_value()) {
        return std::nullopt;
    }
    if (*pos >= text.size() || text[*pos] != '[') {
        return std::nullopt;
    }
    ++(*pos);

    std::vector<std::string> values;
    while (*pos < text.size()) {
        SkipJsonWhitespace(text, *pos);
        if (*pos < text.size() && text[*pos] == ']') {
            ++(*pos);
            return values;
        }
        std::string value;
        if (!ParseJsonStringAt(text, *pos, value)) {
            return std::nullopt;
        }
        if (!value.empty()) {
            values.push_back(std::move(value));
        }
        SkipJsonWhitespace(text, *pos);
        if (*pos < text.size() && text[*pos] == ',') {
            ++(*pos);
            continue;
        }
        if (*pos < text.size() && text[*pos] == ']') {
            ++(*pos);
            return values;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> JsonObjectArrayField(const std::string& text, const std::string& key) {
    auto pos = FindJsonValueStart(text, key);
    if (!pos.has_value()) {
        return std::nullopt;
    }
    if (*pos >= text.size() || text[*pos] != '[') {
        return std::nullopt;
    }
    ++(*pos);

    std::vector<std::string> values;
    while (*pos < text.size()) {
        SkipJsonWhitespace(text, *pos);
        if (*pos < text.size() && text[*pos] == ']') {
            ++(*pos);
            return values;
        }
        if (*pos >= text.size() || text[*pos] != '{') {
            return std::nullopt;
        }
        auto object_pos = *pos;
        auto object = JsonObjectRawAt(text, object_pos);
        if (!object.has_value()) {
            return std::nullopt;
        }
        values.push_back(std::move(*object));
        *pos = object_pos;
        SkipJsonWhitespace(text, *pos);
        if (*pos < text.size() && text[*pos] == ',') {
            ++(*pos);
            continue;
        }
        if (*pos < text.size() && text[*pos] == ']') {
            ++(*pos);
            return values;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> JsonNumberTokenField(const std::string& text, const std::string& key) {
    auto pos = FindJsonValueStart(text, key);
    if (!pos.has_value()) {
        return std::nullopt;
    }
    return JsonNumberTokenAt(text, *pos);
}

std::optional<std::string> JsonNumberTokenAt(const std::string& text, std::size_t pos) {
    const auto start = pos;
    while (pos < text.size()) {
        const char ch = text[pos];
        if ((ch < '0' || ch > '9') && ch != '-' && ch != '+') {
            break;
        }
        ++pos;
    }
    if (pos == start) {
        return std::nullopt;
    }
    return text.substr(start, pos - start);
}

std::optional<int> JsonIntField(const std::string& text, const std::string& key) {
    const auto token = JsonNumberTokenField(text, key);
    if (!token.has_value()) {
        return std::nullopt;
    }
    int value = 0;
    if (!ParseStrictInt(*token, value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<double> JsonDoubleField(const std::string& text, const std::string& key) {
    const auto token = JsonNumberTokenField(text, key);
    if (!token.has_value()) {
        return std::nullopt;
    }
    try {
        std::size_t parsed = 0;
        const auto value = std::stod(*token, &parsed);
        if (parsed == token->size()) {
            return value;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<bool> JsonBoolField(const std::string& text, const std::string& key) {
    auto pos = FindJsonValueStart(text, key);
    if (!pos.has_value()) {
        return std::nullopt;
    }
    if (text.compare(*pos, 4, "true") == 0) {
        return true;
    }
    if (text.compare(*pos, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> JsonObjectRawField(const std::string& text, const std::string& key) {
    auto pos = FindJsonValueStart(text, key);
    if (!pos.has_value() || *pos >= text.size() || text[*pos] != '{') {
        return std::nullopt;
    }
    return JsonObjectRawAt(text, *pos);
}

std::optional<std::string> JsonObjectRawAt(const std::string& text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '{') {
        return std::nullopt;
    }
    const auto start = pos;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    while (pos < text.size()) {
        const char ch = text[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            ++pos;
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            ++pos;
            if (depth == 0) {
                return text.substr(start, pos - start);
            }
            continue;
        }
        ++pos;
    }
    return std::nullopt;
}

std::optional<std::string> JsonSchemaField(const std::string& text, const std::string& key) {
    if (auto string_value = JsonStringField(text, key); string_value.has_value()) {
        return string_value;
    }
    return JsonObjectRawField(text, key);
}

}  // namespace agentos
