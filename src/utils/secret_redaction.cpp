#include "utils/secret_redaction.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace agentos {

namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void ReplaceAll(std::string& value, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) {
        return;
    }

    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

}  // namespace

bool IsSensitiveFieldName(const std::string& name) {
    const auto lower = ToLower(name);
    constexpr std::array<const char*, 9> sensitive_terms = {
        "api_key",
        "apikey",
        "access_key",
        "secret",
        "token",
        "password",
        "passwd",
        "credential",
        "authorization",
    };

    return std::any_of(sensitive_terms.begin(), sensitive_terms.end(), [&](const char* term) {
        return lower.find(term) != std::string::npos;
    });
}

std::vector<std::string> SensitiveValuesFromMap(const StringMap& values) {
    std::vector<std::string> sensitive_values;
    for (const auto& [name, value] : values) {
        if (IsSensitiveFieldName(name) && !value.empty()) {
            sensitive_values.push_back(value);
        }
    }
    return sensitive_values;
}

std::string RedactSensitiveText(std::string text, const std::vector<std::string>& sensitive_values) {
    for (const auto& sensitive_value : sensitive_values) {
        ReplaceAll(text, sensitive_value, "[REDACTED]");
    }
    return text;
}

std::string RedactSensitiveText(std::string text, const StringMap& values) {
    return RedactSensitiveText(std::move(text), SensitiveValuesFromMap(values));
}

}  // namespace agentos
