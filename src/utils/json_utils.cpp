#include "utils/json_utils.hpp"

#include <iomanip>
#include <sstream>

namespace agentos {

std::string EscapeJson(const std::string_view value) {
    std::ostringstream stream;

    for (const char ch : value) {
        switch (ch) {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
            } else {
                stream << ch;
            }
            break;
        }
    }

    return stream.str();
}

std::string QuoteJson(const std::string_view value) {
    return "\"" + EscapeJson(value) + "\"";
}

std::string BoolAsJson(const bool value) {
    return value ? "true" : "false";
}

std::string NumberAsJson(const int value) {
    return std::to_string(value);
}

std::string NumberAsJson(const long long value) {
    return std::to_string(value);
}

std::string NumberAsJson(const double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

std::string MakeJsonObject(const std::initializer_list<std::pair<std::string, std::string>> fields) {
    std::ostringstream stream;
    stream << "{";

    bool first = true;
    for (const auto& [key, value] : fields) {
        if (!first) {
            stream << ",";
        }
        first = false;
        stream << QuoteJson(key) << ":" << value;
    }

    stream << "}";
    return stream.str();
}

}  // namespace agentos

