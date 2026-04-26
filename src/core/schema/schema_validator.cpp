#include "core/schema/schema_validator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

namespace agentos {

namespace {

void SkipWhitespace(const std::string& value, std::size_t& index) {
    while (index < value.size() && std::isspace(static_cast<unsigned char>(value[index])) != 0) {
        ++index;
    }
}

bool ParseJsonString(const std::string& value, std::size_t& index, std::string& output) {
    if (index >= value.size() || value[index] != '"') {
        return false;
    }
    ++index;

    output.clear();
    while (index < value.size()) {
        const char ch = value[index++];
        if (ch == '"') {
            return true;
        }
        if (ch == '\\') {
            if (index >= value.size()) {
                return false;
            }
            output.push_back(value[index++]);
            continue;
        }
        output.push_back(ch);
    }

    return false;
}

bool SkipJsonArray(const std::string& value, std::size_t& index) {
    if (index >= value.size() || value[index] != '[') {
        return false;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; index < value.size(); ++index) {
        const char ch = value[index];
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
            continue;
        }
        if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                ++index;
                return true;
            }
        }
    }
    return false;
}

std::optional<std::size_t> FindMatchingBracket(const std::string& value, const std::size_t open_bracket) {
    if (open_bracket >= value.size() || value[open_bracket] != '[') {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = open_bracket; index < value.size(); ++index) {
        const char ch = value[index];
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
            continue;
        }
        if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::nullopt;
}

bool SkipJsonValue(const std::string& value, std::size_t& index);

std::vector<std::string> ExtractRequiredFields(const std::string& schema_json) {
    std::vector<std::string> fields;
    std::size_t index = 0;
    SkipWhitespace(schema_json, index);
    if (index >= schema_json.size() || schema_json[index] != '{') {
        return fields;
    }
    ++index;

    while (index < schema_json.size()) {
        SkipWhitespace(schema_json, index);
        if (index < schema_json.size() && schema_json[index] == '}') {
            return fields;
        }
        if (index >= schema_json.size() || schema_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(schema_json, index, key)) {
            return {};
        }

        SkipWhitespace(schema_json, index);
        if (index >= schema_json.size() || schema_json[index] != ':') {
            return {};
        }
        ++index;
        SkipWhitespace(schema_json, index);
        if (key != "required") {
            if (!SkipJsonValue(schema_json, index)) {
                return {};
            }
            SkipWhitespace(schema_json, index);
            if (index < schema_json.size() && schema_json[index] == ',') {
                ++index;
            }
            continue;
        }

        if (index >= schema_json.size() || schema_json[index] != '[') {
            return {};
        }
        ++index;

        while (index < schema_json.size()) {
            SkipWhitespace(schema_json, index);
            if (index < schema_json.size() && schema_json[index] == ']') {
                ++index;
                break;
            }

            std::string field;
            if (!ParseJsonString(schema_json, index, field)) {
                return {};
            }
            if (!field.empty()) {
                fields.push_back(field);
            }

            SkipWhitespace(schema_json, index);
            if (index < schema_json.size() && schema_json[index] == ',') {
                ++index;
                continue;
            }
            if (index < schema_json.size() && schema_json[index] == ']') {
                ++index;
                break;
            }
            return {};
        }

        SkipWhitespace(schema_json, index);
        if (index < schema_json.size() && schema_json[index] == ',') {
            ++index;
        }
    }

    return fields;
}

std::optional<std::size_t> FindMatchingBrace(const std::string& value, const std::size_t open_brace) {
    if (open_brace >= value.size() || value[open_brace] != '{') {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = open_brace; index < value.size(); ++index) {
        const char ch = value[index];
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
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> ExtractTypeFromPropertyObject(const std::string& object_json) {
    std::size_t index = 0;
    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return std::nullopt;
        }
        if (key != "type") {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(object_json, index);

        std::string type;
        if (!ParseJsonString(object_json, index, type)) {
            return std::nullopt;
        }
        return type;
    }

    return std::nullopt;
}

bool SkipJsonValue(const std::string& value, std::size_t& index) {
    SkipWhitespace(value, index);
    if (index >= value.size()) {
        return false;
    }

    if (value[index] == '{') {
        const auto object_end = FindMatchingBrace(value, index);
        if (!object_end.has_value()) {
            return false;
        }
        index = *object_end + 1;
        return true;
    }
    if (value[index] == '[') {
        return SkipJsonArray(value, index);
    }
    if (value[index] == '"') {
        std::string ignored;
        return ParseJsonString(value, index, ignored);
    }
    while (index < value.size() &&
           value[index] != ',' &&
           value[index] != '}' &&
           value[index] != ']') {
        ++index;
    }
    return true;
}

std::optional<std::string> ExtractObjectKeyword(const std::string& object_json, const std::string& keyword) {
    std::size_t index = 0;
    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return std::nullopt;
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != '{') {
            return std::nullopt;
        }

        const auto object_end = FindMatchingBrace(object_json, index);
        if (!object_end.has_value()) {
            return std::nullopt;
        }
        return object_json.substr(index, *object_end - index + 1);
    }

    return std::nullopt;
}

std::optional<std::string> ExtractArrayKeyword(const std::string& object_json, const std::string& keyword) {
    std::size_t index = 0;
    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return std::nullopt;
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != '[') {
            return std::nullopt;
        }

        const auto array_end = FindMatchingBracket(object_json, index);
        if (!array_end.has_value()) {
            return std::nullopt;
        }
        return object_json.substr(index, *array_end - index + 1);
    }

    return std::nullopt;
}

std::optional<std::string> ExtractPropertyObject(
    const std::string& schema_json,
    const std::string& property_name) {
    const auto properties_object = ExtractObjectKeyword(schema_json, "properties");
    if (!properties_object.has_value()) {
        return std::nullopt;
    }

    std::size_t index = 0;
    while (index < properties_object->size()) {
        if ((*properties_object)[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(*properties_object, index, key)) {
            return std::nullopt;
        }
        SkipWhitespace(*properties_object, index);
        if (index >= properties_object->size() || (*properties_object)[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(*properties_object, index);
        if (key != property_name) {
            if (!SkipJsonValue(*properties_object, index)) {
                return std::nullopt;
            }
            continue;
        }
        if (index >= properties_object->size() || (*properties_object)[index] != '{') {
            return std::nullopt;
        }
        const auto property_end = FindMatchingBrace(*properties_object, index);
        if (!property_end.has_value()) {
            return std::nullopt;
        }
        return properties_object->substr(index, *property_end - index + 1);
    }

    return std::nullopt;
}

std::optional<std::string> ExtractStringKeyword(const std::string& object_json, const std::string& keyword) {
    std::size_t index = 0;
    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return std::nullopt;
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(object_json, index);

        std::string value;
        if (!ParseJsonString(object_json, index, value)) {
            return std::nullopt;
        }
        return value;
    }

    return std::nullopt;
}

std::optional<double> ExtractNumberKeyword(const std::string& object_json, const std::string& keyword) {
    std::size_t index = 0;
    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return std::nullopt;
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(object_json, index);

        std::size_t parsed = 0;
        try {
            const auto number = std::stod(object_json.substr(index), &parsed);
            if (parsed == 0) {
                return std::nullopt;
            }
            return number;
        } catch (...) {
            return std::nullopt;
        }
    }

    return std::nullopt;
}

std::optional<bool> ExtractBooleanKeyword(const std::string& object_json, const std::string& keyword) {
    std::size_t index = 0;
    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return std::nullopt;
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(object_json, index);

        if (object_json.compare(index, 4, "true") == 0) {
            return true;
        }
        if (object_json.compare(index, 5, "false") == 0) {
            return false;
        }
        return std::nullopt;
    }

    return std::nullopt;
}

std::optional<int> ExtractIntegerKeyword(const std::string& object_json, const std::string& keyword) {
    if (const auto number = ExtractNumberKeyword(object_json, keyword); number.has_value()) {
        return static_cast<int>(*number);
    }
    return std::nullopt;
}

std::vector<std::string> ExtractStringArrayKeyword(const std::string& object_json, const std::string& keyword) {
    std::vector<std::string> values;
    std::size_t index = 0;

    while (index < object_json.size()) {
        if (object_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(object_json, index, key)) {
            return {};
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            return {};
        }
        ++index;
        SkipWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != '[') {
            return {};
        }
        ++index;

        while (index < object_json.size()) {
            SkipWhitespace(object_json, index);
            if (index < object_json.size() && object_json[index] == ']') {
                return values;
            }

            std::string value;
            if (!ParseJsonString(object_json, index, value)) {
                return {};
            }
            values.push_back(value);

            SkipWhitespace(object_json, index);
            if (index < object_json.size() && object_json[index] == ',') {
                ++index;
                continue;
            }
            if (index < object_json.size() && object_json[index] == ']') {
                return values;
            }
            return {};
        }
    }

    return values;
}

struct PropertySchema {
    std::string name;
    std::optional<std::string> type;
    std::optional<std::string> const_value;
    std::vector<std::string> enum_values;
    std::optional<int> min_length;
    std::optional<int> max_length;
    std::optional<std::string> pattern;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> exclusive_minimum;
    std::optional<double> exclusive_maximum;
    std::optional<double> multiple_of;
};

struct DependentRequiredSchema {
    std::string field;
    std::vector<std::string> required_fields;
};

PropertySchema ParsePropertySchema(const std::string& name, const std::string& object_json) {
    return {
        .name = name,
        .type = ExtractTypeFromPropertyObject(object_json),
        .const_value = ExtractStringKeyword(object_json, "const"),
        .enum_values = ExtractStringArrayKeyword(object_json, "enum"),
        .min_length = ExtractIntegerKeyword(object_json, "minLength"),
        .max_length = ExtractIntegerKeyword(object_json, "maxLength"),
        .pattern = ExtractStringKeyword(object_json, "pattern"),
        .minimum = ExtractNumberKeyword(object_json, "minimum"),
        .maximum = ExtractNumberKeyword(object_json, "maximum"),
        .exclusive_minimum = ExtractNumberKeyword(object_json, "exclusiveMinimum"),
        .exclusive_maximum = ExtractNumberKeyword(object_json, "exclusiveMaximum"),
        .multiple_of = ExtractNumberKeyword(object_json, "multipleOf"),
    };
}

std::vector<PropertySchema> ExtractPropertySchemas(const std::string& schema_json) {
    std::vector<PropertySchema> property_schemas;
    std::size_t index = 0;

    while (index < schema_json.size()) {
        if (schema_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(schema_json, index, key)) {
            return {};
        }
        if (key != "properties") {
            continue;
        }

        SkipWhitespace(schema_json, index);
        if (index >= schema_json.size() || schema_json[index] != ':') {
            return {};
        }
        ++index;
        SkipWhitespace(schema_json, index);
        if (index >= schema_json.size() || schema_json[index] != '{') {
            return {};
        }

        const auto properties_end = FindMatchingBrace(schema_json, index);
        if (!properties_end.has_value()) {
            return {};
        }

        ++index;
        while (index < *properties_end) {
            SkipWhitespace(schema_json, index);
            if (index >= *properties_end || schema_json[index] == '}') {
                break;
            }

            std::string property_name;
            if (!ParseJsonString(schema_json, index, property_name)) {
                return {};
            }
            SkipWhitespace(schema_json, index);
            if (index >= *properties_end || schema_json[index] != ':') {
                return {};
            }
            ++index;
            SkipWhitespace(schema_json, index);
            if (index >= *properties_end || schema_json[index] != '{') {
                return {};
            }

            const auto property_end = FindMatchingBrace(schema_json, index);
            if (!property_end.has_value() || *property_end > *properties_end) {
                return {};
            }

            property_schemas.push_back(ParsePropertySchema(
                property_name,
                schema_json.substr(index, *property_end - index + 1)));

            index = *property_end + 1;
            SkipWhitespace(schema_json, index);
            if (index < *properties_end && schema_json[index] == ',') {
                ++index;
            }
        }

        return property_schemas;
    }

    return property_schemas;
}

std::vector<DependentRequiredSchema> ExtractDependentRequiredSchemas(
    const std::string& schema_json,
    const std::string& keyword) {
    std::vector<DependentRequiredSchema> dependencies;
    std::size_t index = 0;

    while (index < schema_json.size()) {
        if (schema_json[index] != '"') {
            ++index;
            continue;
        }

        std::string key;
        if (!ParseJsonString(schema_json, index, key)) {
            return {};
        }
        if (key != keyword) {
            continue;
        }

        SkipWhitespace(schema_json, index);
        if (index >= schema_json.size() || schema_json[index] != ':') {
            return {};
        }
        ++index;
        SkipWhitespace(schema_json, index);
        if (index >= schema_json.size() || schema_json[index] != '{') {
            return {};
        }

        const auto dependencies_end = FindMatchingBrace(schema_json, index);
        if (!dependencies_end.has_value()) {
            return {};
        }

        ++index;
        while (index < *dependencies_end) {
            SkipWhitespace(schema_json, index);
            if (index >= *dependencies_end || schema_json[index] == '}') {
                break;
            }

            std::string field_name;
            if (!ParseJsonString(schema_json, index, field_name)) {
                return {};
            }
            SkipWhitespace(schema_json, index);
            if (index >= *dependencies_end || schema_json[index] != ':') {
                return {};
            }
            ++index;
            SkipWhitespace(schema_json, index);
            if (index >= *dependencies_end || schema_json[index] != '[') {
                return {};
            }
            ++index;

            std::vector<std::string> required_fields;
            while (index < *dependencies_end) {
                SkipWhitespace(schema_json, index);
                if (index < *dependencies_end && schema_json[index] == ']') {
                    ++index;
                    break;
                }

                std::string required_field;
                if (!ParseJsonString(schema_json, index, required_field)) {
                    return {};
                }
                if (!required_field.empty()) {
                    required_fields.push_back(required_field);
                }

                SkipWhitespace(schema_json, index);
                if (index < *dependencies_end && schema_json[index] == ',') {
                    ++index;
                    continue;
                }
                if (index < *dependencies_end && schema_json[index] == ']') {
                    ++index;
                    break;
                }
                return {};
            }

            dependencies.push_back(DependentRequiredSchema{
                .field = field_name,
                .required_fields = std::move(required_fields),
            });

            SkipWhitespace(schema_json, index);
            if (index < *dependencies_end && schema_json[index] == ',') {
                ++index;
            }
        }

        return dependencies;
    }

    return dependencies;
}

std::vector<std::string> ExtractNotRequiredFields(const std::string& schema_json) {
    const auto not_schema = ExtractObjectKeyword(schema_json, "not");
    if (!not_schema.has_value()) {
        return {};
    }
    return ExtractRequiredFields(*not_schema);
}

std::optional<std::pair<std::string, std::string>> ExtractIfPropertyConstCondition(const std::string& schema_json) {
    const auto if_schema = ExtractObjectKeyword(schema_json, "if");
    if (!if_schema.has_value()) {
        return std::nullopt;
    }

    const auto if_properties = ExtractObjectKeyword(*if_schema, "properties");
    if (!if_properties.has_value()) {
        return std::nullopt;
    }

    std::size_t index = 0;
    while (index < if_properties->size()) {
        if ((*if_properties)[index] != '"') {
            ++index;
            continue;
        }

        std::string property_name;
        if (!ParseJsonString(*if_properties, index, property_name)) {
            return std::nullopt;
        }
        SkipWhitespace(*if_properties, index);
        if (index >= if_properties->size() || (*if_properties)[index] != ':') {
            return std::nullopt;
        }
        ++index;
        SkipWhitespace(*if_properties, index);
        if (index >= if_properties->size() || (*if_properties)[index] != '{') {
            return std::nullopt;
        }

        const auto property_end = FindMatchingBrace(*if_properties, index);
        if (!property_end.has_value()) {
            return std::nullopt;
        }
        const auto property_schema = if_properties->substr(index, *property_end - index + 1);
        if (const auto const_value = ExtractStringKeyword(property_schema, "const"); const_value.has_value()) {
            return std::make_pair(property_name, *const_value);
        }

        index = *property_end + 1;
    }

    return std::nullopt;
}

std::vector<std::vector<std::string>> ExtractRequiredBranches(
    const std::string& schema_json,
    const std::string& keyword) {
    std::vector<std::vector<std::string>> branches;
    const auto array_json = ExtractArrayKeyword(schema_json, keyword);
    if (!array_json.has_value()) {
        return branches;
    }

    std::size_t index = 1;
    while (index < array_json->size()) {
        SkipWhitespace(*array_json, index);
        if (index >= array_json->size() || (*array_json)[index] == ']') {
            break;
        }
        if ((*array_json)[index] != '{') {
            return {};
        }

        const auto branch_end = FindMatchingBrace(*array_json, index);
        if (!branch_end.has_value()) {
            return {};
        }
        branches.push_back(ExtractRequiredFields(array_json->substr(index, *branch_end - index + 1)));
        index = *branch_end + 1;
        SkipWhitespace(*array_json, index);
        if (index < array_json->size() && (*array_json)[index] == ',') {
            ++index;
        }
    }

    return branches;
}

int CountMatchingRequiredBranches(
    const std::vector<std::vector<std::string>>& branches,
    const StringMap& arguments) {
    int matches = 0;
    for (const auto& branch : branches) {
        if (branch.empty()) {
            continue;
        }
        const bool matched = std::all_of(branch.begin(), branch.end(), [&](const std::string& field) {
            return arguments.contains(field);
        });
        if (matched) {
            ++matches;
        }
    }
    return matches;
}

bool AllRequiredBranchesMatch(
    const std::vector<std::vector<std::string>>& branches,
    const StringMap& arguments) {
    for (const auto& branch : branches) {
        if (branch.empty()) {
            continue;
        }
        const bool matched = std::all_of(branch.begin(), branch.end(), [&](const std::string& field) {
            return arguments.contains(field);
        });
        if (!matched) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> ValidatePropertyNameConstraints(
    const std::string& property_name,
    const std::string& property_names_schema) {
    std::vector<std::string> failures;

    if (const auto min_length = ExtractIntegerKeyword(property_names_schema, "minLength");
        min_length.has_value() && property_name.size() < static_cast<std::size_t>(*min_length)) {
        failures.push_back("schema:propertyNames:minLength");
    }
    if (const auto max_length = ExtractIntegerKeyword(property_names_schema, "maxLength");
        max_length.has_value() && property_name.size() > static_cast<std::size_t>(*max_length)) {
        failures.push_back("schema:propertyNames:maxLength");
    }
    if (const auto pattern = ExtractStringKeyword(property_names_schema, "pattern"); pattern.has_value()) {
        try {
            if (!std::regex_search(property_name, std::regex(*pattern))) {
                failures.push_back("schema:propertyNames:pattern");
            }
        } catch (const std::regex_error&) {
            failures.push_back("schema:propertyNames:pattern");
        }
    }
    if (const auto const_value = ExtractStringKeyword(property_names_schema, "const");
        const_value.has_value() && property_name != *const_value) {
        failures.push_back("schema:propertyNames:const");
    }
    const auto enum_values = ExtractStringArrayKeyword(property_names_schema, "enum");
    if (!enum_values.empty() &&
        std::find(enum_values.begin(), enum_values.end(), property_name) == enum_values.end()) {
        failures.push_back("schema:propertyNames:enum");
    }

    return failures;
}

bool IsJsonNumberString(const std::string& value) {
    std::size_t parsed = 0;
    try {
        (void)std::stod(value, &parsed);
    } catch (...) {
        return false;
    }
    return parsed == value.size();
}

bool IsJsonIntegerString(const std::string& value) {
    std::size_t parsed = 0;
    try {
        (void)std::stoll(value, &parsed);
    } catch (...) {
        return false;
    }
    return parsed == value.size();
}

bool IsJsonBooleanString(const std::string& value) {
    return value == "true" || value == "false" || value == "1" || value == "0";
}

bool ArgumentMatchesSchemaType(const std::string& value, const std::string& type) {
    if (type == "string") {
        return true;
    }
    if (type == "number") {
        return IsJsonNumberString(value);
    }
    if (type == "integer") {
        return IsJsonIntegerString(value);
    }
    if (type == "boolean") {
        return IsJsonBooleanString(value);
    }
    return true;
}

std::optional<double> ParseArgumentNumber(const std::string& value) {
    std::size_t parsed = 0;
    try {
        const auto number = std::stod(value, &parsed);
        if (parsed == value.size()) {
            return number;
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::vector<std::string> ValidatePropertyConstraints(const std::string& value, const PropertySchema& schema) {
    std::vector<std::string> failures;

    if (!schema.enum_values.empty() &&
        std::find(schema.enum_values.begin(), schema.enum_values.end(), value) == schema.enum_values.end()) {
        failures.push_back(schema.name + ":enum");
    }
    if (schema.const_value.has_value() && value != *schema.const_value) {
        failures.push_back(schema.name + ":const");
    }
    if (schema.min_length.has_value() && value.size() < static_cast<std::size_t>(*schema.min_length)) {
        failures.push_back(schema.name + ":minLength");
    }
    if (schema.max_length.has_value() && value.size() > static_cast<std::size_t>(*schema.max_length)) {
        failures.push_back(schema.name + ":maxLength");
    }
    if (schema.pattern.has_value()) {
        try {
            if (!std::regex_search(value, std::regex(*schema.pattern))) {
                failures.push_back(schema.name + ":pattern");
            }
        } catch (const std::regex_error&) {
            failures.push_back(schema.name + ":pattern");
        }
    }

    const auto number = ParseArgumentNumber(value);
    if (schema.minimum.has_value() && (!number.has_value() || *number < *schema.minimum)) {
        failures.push_back(schema.name + ":minimum");
    }
    if (schema.maximum.has_value() && (!number.has_value() || *number > *schema.maximum)) {
        failures.push_back(schema.name + ":maximum");
    }
    if (schema.exclusive_minimum.has_value() && (!number.has_value() || *number <= *schema.exclusive_minimum)) {
        failures.push_back(schema.name + ":exclusiveMinimum");
    }
    if (schema.exclusive_maximum.has_value() && (!number.has_value() || *number >= *schema.exclusive_maximum)) {
        failures.push_back(schema.name + ":exclusiveMaximum");
    }
    if (schema.multiple_of.has_value()) {
        if (!number.has_value() || *schema.multiple_of <= 0.0) {
            failures.push_back(schema.name + ":multipleOf");
        } else {
            const auto remainder = std::fmod(std::fabs(*number), *schema.multiple_of);
            constexpr double kMultipleOfTolerance = 1e-9;
            if (remainder > kMultipleOfTolerance &&
                std::fabs(remainder - *schema.multiple_of) > kMultipleOfTolerance) {
                failures.push_back(schema.name + ":multipleOf");
            }
        }
    }

    return failures;
}

std::string JoinFields(const std::vector<std::string>& fields) {
    std::ostringstream output;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << fields[index];
    }
    return output.str();
}

bool HasPropertySchema(const std::vector<PropertySchema>& property_schemas, const std::string& name) {
    return std::any_of(
        property_schemas.begin(),
        property_schemas.end(),
        [&](const PropertySchema& schema) {
            return schema.name == name;
        });
}

}  // namespace

SchemaValidationResult ValidateRequiredInputFields(
    const SkillManifest& manifest,
    const StringMap& arguments) {
    SchemaValidationResult result;

    const auto required_fields = ExtractRequiredFields(manifest.input_schema_json);
    for (const auto& field : required_fields) {
        if (!arguments.contains(field)) {
            result.missing_required_fields.push_back(field);
        }
    }

    const auto property_schemas = ExtractPropertySchemas(manifest.input_schema_json);
    if (const auto property_names_schema = ExtractObjectKeyword(manifest.input_schema_json, "propertyNames");
        property_names_schema.has_value()) {
        for (const auto& [argument_name, _] : arguments) {
            for (const auto& failure : ValidatePropertyNameConstraints(argument_name, *property_names_schema)) {
                result.invalid_constraint_fields.push_back(failure + ":" + argument_name);
            }
        }
    }

    if (const auto min_properties = ExtractIntegerKeyword(manifest.input_schema_json, "minProperties");
        min_properties.has_value() && arguments.size() < static_cast<std::size_t>(*min_properties)) {
        result.invalid_constraint_fields.push_back("schema:minProperties");
    }
    if (const auto max_properties = ExtractIntegerKeyword(manifest.input_schema_json, "maxProperties");
        max_properties.has_value() && arguments.size() > static_cast<std::size_t>(*max_properties)) {
        result.invalid_constraint_fields.push_back("schema:maxProperties");
    }

    for (const auto& dependency_group : {
        ExtractDependentRequiredSchemas(manifest.input_schema_json, "dependentRequired"),
        ExtractDependentRequiredSchemas(manifest.input_schema_json, "dependencies"),
    }) {
        for (const auto& dependency : dependency_group) {
            if (!arguments.contains(dependency.field)) {
                continue;
            }
            for (const auto& required_field : dependency.required_fields) {
                if (!arguments.contains(required_field)) {
                    result.invalid_constraint_fields.push_back(
                        dependency.field + ":dependentRequired:" + required_field);
                }
            }
        }
    }

    const auto not_required_fields = ExtractNotRequiredFields(manifest.input_schema_json);
    if (!not_required_fields.empty() &&
        std::all_of(not_required_fields.begin(), not_required_fields.end(), [&](const std::string& field) {
            return arguments.contains(field);
        })) {
        result.invalid_constraint_fields.push_back("schema:not:required");
    }

    if (const auto if_condition = ExtractIfPropertyConstCondition(manifest.input_schema_json); if_condition.has_value()) {
        const auto input = arguments.find(if_condition->first);
        const bool condition_matched = input != arguments.end() && input->second == if_condition->second;
        const auto branch_schema = ExtractObjectKeyword(
            manifest.input_schema_json,
            condition_matched ? "then" : "else");
        if (branch_schema.has_value()) {
            for (const auto& required_field : ExtractRequiredFields(*branch_schema)) {
                if (!arguments.contains(required_field)) {
                    result.invalid_constraint_fields.push_back(
                        std::string(condition_matched ? "schema:then:required:" : "schema:else:required:") +
                        required_field);
                }
            }
        }
    }

    const auto any_of_required_branches = ExtractRequiredBranches(manifest.input_schema_json, "anyOf");
    if (!any_of_required_branches.empty() &&
        CountMatchingRequiredBranches(any_of_required_branches, arguments) == 0) {
        result.invalid_constraint_fields.push_back("schema:anyOf:required");
    }

    const auto one_of_required_branches = ExtractRequiredBranches(manifest.input_schema_json, "oneOf");
    if (!one_of_required_branches.empty() &&
        CountMatchingRequiredBranches(one_of_required_branches, arguments) != 1) {
        result.invalid_constraint_fields.push_back("schema:oneOf:required");
    }

    const auto all_of_required_branches = ExtractRequiredBranches(manifest.input_schema_json, "allOf");
    if (!all_of_required_branches.empty() &&
        !AllRequiredBranchesMatch(all_of_required_branches, arguments)) {
        result.invalid_constraint_fields.push_back("schema:allOf:required");
    }

    if (const auto additional_properties = ExtractBooleanKeyword(manifest.input_schema_json, "additionalProperties");
        additional_properties.has_value() && !*additional_properties) {
        for (const auto& [argument_name, _] : arguments) {
            if (!HasPropertySchema(property_schemas, argument_name)) {
                result.invalid_constraint_fields.push_back(argument_name + ":additionalProperties");
            }
        }
    }

    for (const auto& property_schema : property_schemas) {
        const auto argument = arguments.find(property_schema.name);
        if (argument == arguments.end()) {
            continue;
        }

        if (property_schema.type.has_value() &&
            !ArgumentMatchesSchemaType(argument->second, *property_schema.type)) {
            result.invalid_type_fields.push_back(property_schema.name + ":" + *property_schema.type);
            continue;
        }

        for (const auto& failure : ValidatePropertyConstraints(argument->second, property_schema)) {
            result.invalid_constraint_fields.push_back(failure);
        }
    }

    if (!result.missing_required_fields.empty()) {
        result.valid = false;
        result.error_message = "missing required input fields for " + manifest.name + ": " +
                               JoinFields(result.missing_required_fields);
    } else if (!result.invalid_type_fields.empty()) {
        result.valid = false;
        result.error_message = "invalid input field types for " + manifest.name + ": " +
                               JoinFields(result.invalid_type_fields);
    } else if (!result.invalid_constraint_fields.empty()) {
        result.valid = false;
        result.error_message = "invalid input field constraints for " + manifest.name + ": " +
                               JoinFields(result.invalid_constraint_fields);
    }

    return result;
}

}  // namespace agentos
