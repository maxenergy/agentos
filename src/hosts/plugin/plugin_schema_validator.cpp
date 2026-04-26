#include "hosts/plugin/plugin_schema_validator.hpp"

#include "hosts/plugin/plugin_host.hpp"
#include "hosts/plugin/plugin_json_utils.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <utility>
#include <vector>

namespace agentos {
namespace {

std::vector<std::string> JsonObjectFieldNames(const std::string& object_json) {
    std::vector<std::string> fields;
    for (std::size_t index = 0; index < object_json.size();) {
        SkipJsonWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] == '}') {
            break;
        }
        if (object_json[index] == '{' || object_json[index] == ',') {
            ++index;
            continue;
        }
        if (object_json[index] != '"') {
            ++index;
            continue;
        }
        std::string field;
        if (!ParseJsonStringAt(object_json, index, field)) {
            break;
        }
        fields.push_back(std::move(field));
        SkipJsonWhitespace(object_json, index);
        if (index >= object_json.size() || object_json[index] != ':') {
            break;
        }
        ++index;
        SkipJsonWhitespace(object_json, index);
        if (index < object_json.size() && object_json[index] == '{') {
            (void)JsonObjectRawAt(object_json, index);
        } else if (index < object_json.size() && object_json[index] == '"') {
            std::string ignored;
            (void)ParseJsonStringAt(object_json, index, ignored);
        } else if (index < object_json.size() && object_json[index] == '[') {
            int depth = 0;
            bool in_string = false;
            bool escaped = false;
            while (index < object_json.size()) {
                const char ch = object_json[index++];
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
                } else if (ch == '[') {
                    ++depth;
                } else if (ch == ']') {
                    --depth;
                    if (depth == 0) {
                        break;
                    }
                }
            }
        } else {
            while (index < object_json.size() && object_json[index] != ',' && object_json[index] != '}') {
                ++index;
            }
        }
    }
    return fields;
}

std::string JsonPropertyNameConstraintError(
    const std::string& property_name,
    const std::string& property_names_schema) {
    if (const auto min_length = JsonIntField(property_names_schema, "minLength");
        min_length.has_value() && property_name.size() < static_cast<std::size_t>(*min_length)) {
        return "plugin output field has invalid constraint: " + property_name + ":propertyNames:minLength";
    }
    if (const auto max_length = JsonIntField(property_names_schema, "maxLength");
        max_length.has_value() && property_name.size() > static_cast<std::size_t>(*max_length)) {
        return "plugin output field has invalid constraint: " + property_name + ":propertyNames:maxLength";
    }
    if (const auto pattern = JsonStringField(property_names_schema, "pattern"); pattern.has_value()) {
        try {
            if (!std::regex_search(property_name, std::regex(*pattern))) {
                return "plugin output field has invalid constraint: " + property_name + ":propertyNames:pattern";
            }
        } catch (const std::regex_error&) {
            return "plugin output field has invalid constraint: " + property_name + ":propertyNames:pattern";
        }
    }
    if (const auto const_value = JsonStringField(property_names_schema, "const");
        const_value.has_value() && property_name != *const_value) {
        return "plugin output field has invalid constraint: " + property_name + ":propertyNames:const";
    }
    const auto enum_values = JsonStringArrayField(property_names_schema, "enum").value_or(std::vector<std::string>{});
    if (!enum_values.empty() &&
        std::find(enum_values.begin(), enum_values.end(), property_name) == enum_values.end()) {
        return "plugin output field has invalid constraint: " + property_name + ":propertyNames:enum";
    }
    return {};
}

std::string JsonDependentRequiredError(
    const std::string& schema_json,
    const std::string& keyword,
    const std::string& output_json) {
    const auto dependency_object = JsonObjectRawField(schema_json, keyword);
    if (!dependency_object.has_value()) {
        return {};
    }

    for (const auto& field : JsonObjectFieldNames(*dependency_object)) {
        if (!FindJsonValueStart(output_json, field).has_value()) {
            continue;
        }
        const auto required_fields =
            JsonStringArrayField(*dependency_object, field).value_or(std::vector<std::string>{});
        for (const auto& required_field : required_fields) {
            if (!FindJsonValueStart(output_json, required_field).has_value()) {
                return "plugin output field has invalid constraint: " + field +
                    ":dependentRequired:" + required_field;
            }
        }
    }
    return {};
}

std::optional<std::pair<std::string, std::string>> JsonIfPropertyConstCondition(const std::string& schema_json) {
    const auto if_schema = JsonObjectRawField(schema_json, "if");
    if (!if_schema.has_value()) {
        return std::nullopt;
    }
    const auto if_properties = JsonObjectRawField(*if_schema, "properties");
    if (!if_properties.has_value()) {
        return std::nullopt;
    }

    for (const auto& field : JsonObjectFieldNames(*if_properties)) {
        const auto property_schema = JsonObjectRawField(*if_properties, field);
        if (!property_schema.has_value()) {
            continue;
        }
        if (const auto const_value = JsonStringField(*property_schema, "const"); const_value.has_value()) {
            return std::make_pair(field, *const_value);
        }
    }
    return std::nullopt;
}

}  // namespace

std::string PluginOutputSchemaError(const PluginSpec& spec, const std::string& output_json) {
    const auto required_fields = JsonStringArrayField(spec.output_schema_json, "required").value_or(std::vector<std::string>{});
    for (const auto& field : required_fields) {
        if (!FindJsonValueStart(output_json, field).has_value()) {
            return "plugin output missing required field: " + field;
        }
    }

    const auto output_fields = JsonObjectFieldNames(output_json);
    if (const auto min_properties = JsonIntField(spec.output_schema_json, "minProperties");
        min_properties.has_value() && output_fields.size() < static_cast<std::size_t>(*min_properties)) {
        return "plugin output failed minProperties";
    }
    if (const auto max_properties = JsonIntField(spec.output_schema_json, "maxProperties");
        max_properties.has_value() && output_fields.size() > static_cast<std::size_t>(*max_properties)) {
        return "plugin output failed maxProperties";
    }
    if (const auto property_names_schema = JsonObjectRawField(spec.output_schema_json, "propertyNames");
        property_names_schema.has_value()) {
        for (const auto& field : output_fields) {
            if (const auto error = JsonPropertyNameConstraintError(field, *property_names_schema); !error.empty()) {
                return error;
            }
        }
    }
    if (const auto error = JsonDependentRequiredError(spec.output_schema_json, "dependentRequired", output_json);
        !error.empty()) {
        return error;
    }
    if (const auto error = JsonDependentRequiredError(spec.output_schema_json, "dependencies", output_json);
        !error.empty()) {
        return error;
    }
    if (const auto not_schema = JsonObjectRawField(spec.output_schema_json, "not"); not_schema.has_value()) {
        const auto not_required_fields =
            JsonStringArrayField(*not_schema, "required").value_or(std::vector<std::string>{});
        if (!not_required_fields.empty() &&
            std::all_of(not_required_fields.begin(), not_required_fields.end(), [&](const std::string& field) {
                return FindJsonValueStart(output_json, field).has_value();
            })) {
            return "plugin output failed not.required";
        }
    }
    if (const auto if_condition = JsonIfPropertyConstCondition(spec.output_schema_json); if_condition.has_value()) {
        const auto condition_value = JsonStringField(output_json, if_condition->first);
        const bool condition_matched = condition_value.has_value() && *condition_value == if_condition->second;
        const auto branch_schema = JsonObjectRawField(spec.output_schema_json, condition_matched ? "then" : "else");
        if (branch_schema.has_value()) {
            for (const auto& required_field :
                JsonStringArrayField(*branch_schema, "required").value_or(std::vector<std::string>{})) {
                if (!FindJsonValueStart(output_json, required_field).has_value()) {
                    return std::string("plugin output failed ") +
                        (condition_matched ? "then" : "else") +
                        " required field: " + required_field;
                }
            }
        }
    }

    const auto required_branch_matches = [&](const std::vector<std::string>& branch) {
        return std::all_of(branch.begin(), branch.end(), [&](const std::string& field) {
            return FindJsonValueStart(output_json, field).has_value();
        });
    };
    const auto count_matching_required_branches = [&](const std::vector<std::string>& branch_objects) {
        int matches = 0;
        for (const auto& branch_object : branch_objects) {
            const auto branch = JsonStringArrayField(branch_object, "required").value_or(std::vector<std::string>{});
            if (!branch.empty() && required_branch_matches(branch)) {
                ++matches;
            }
        }
        return matches;
    };

    const auto all_of_branches = JsonObjectArrayField(spec.output_schema_json, "allOf").value_or(std::vector<std::string>{});
    if (!all_of_branches.empty()) {
        for (const auto& branch_object : all_of_branches) {
            const auto branch = JsonStringArrayField(branch_object, "required").value_or(std::vector<std::string>{});
            if (!branch.empty() && !required_branch_matches(branch)) {
                return "plugin output failed allOf required branches";
            }
        }
    }
    const auto any_of_branches = JsonObjectArrayField(spec.output_schema_json, "anyOf").value_or(std::vector<std::string>{});
    if (!any_of_branches.empty() && count_matching_required_branches(any_of_branches) == 0) {
        return "plugin output failed anyOf required branches";
    }
    const auto one_of_branches = JsonObjectArrayField(spec.output_schema_json, "oneOf").value_or(std::vector<std::string>{});
    if (!one_of_branches.empty() && count_matching_required_branches(one_of_branches) != 1) {
        return "plugin output failed oneOf required branches";
    }

    const auto properties = JsonObjectRawField(spec.output_schema_json, "properties").value_or("{}");
    std::vector<std::string> declared_properties;
    for (std::size_t index = 0; index < properties.size();) {
        SkipJsonWhitespace(properties, index);
        if (index >= properties.size() || properties[index] == '}') {
            break;
        }
        if (properties[index] == '{' || properties[index] == ',') {
            ++index;
            continue;
        }
        if (properties[index] != '"') {
            ++index;
            continue;
        }

        std::string field;
        if (!ParseJsonStringAt(properties, index, field)) {
            break;
        }
        declared_properties.push_back(field);
        SkipJsonWhitespace(properties, index);
        if (index >= properties.size() || properties[index] != ':') {
            break;
        }
        ++index;
        SkipJsonWhitespace(properties, index);
        if (index >= properties.size() || properties[index] != '{') {
            break;
        }
        const auto field_schema = JsonObjectRawAt(properties, index).value_or("{}");

        const auto type = JsonStringField(field_schema, "type");
        if (!type.has_value()) {
            continue;
        }
        const auto value_start = FindJsonValueStart(output_json, field);
        if (!value_start.has_value()) {
            continue;
        }
        if (*type == "string") {
            std::size_t value_index = *value_start;
            std::string ignored;
            if (!ParseJsonStringAt(output_json, value_index, ignored)) {
                return "plugin output field has invalid type: " + field + ":string";
            }
        } else if (*type == "number") {
            if (!JsonNumberTokenAt(output_json, *value_start).has_value()) {
                return "plugin output field has invalid type: " + field + ":number";
            }
        } else if (*type == "integer") {
            const auto token = JsonNumberTokenAt(output_json, *value_start);
            int ignored = 0;
            if (!token.has_value() || !ParseStrictInt(*token, ignored)) {
                return "plugin output field has invalid type: " + field + ":integer";
            }
        } else if (*type == "boolean") {
            if (output_json.compare(*value_start, 4, "true") != 0 &&
                output_json.compare(*value_start, 5, "false") != 0) {
                return "plugin output field has invalid type: " + field + ":boolean";
            }
        } else if (*type == "object") {
            if (*value_start >= output_json.size() || output_json[*value_start] != '{') {
                return "plugin output field has invalid type: " + field + ":object";
            }
        }

        const auto const_value = JsonStringField(field_schema, "const");
        const auto enum_values = JsonStringArrayField(field_schema, "enum").value_or(std::vector<std::string>{});
        if (const_value.has_value() || !enum_values.empty()) {
            std::size_t value_index = *value_start;
            std::string string_value;
            if (!ParseJsonStringAt(output_json, value_index, string_value)) {
                return "plugin output field has invalid constraint: " + field + ":string-value";
            }
            if (const_value.has_value() && string_value != *const_value) {
                return "plugin output field has invalid constraint: " + field + ":const";
            }
            if (!enum_values.empty() &&
                std::find(enum_values.begin(), enum_values.end(), string_value) == enum_values.end()) {
                return "plugin output field has invalid constraint: " + field + ":enum";
            }
        }
        if (const auto min_length = JsonIntField(field_schema, "minLength"); min_length.has_value()) {
            std::size_t value_index = *value_start;
            std::string string_value;
            if (!ParseJsonStringAt(output_json, value_index, string_value) ||
                string_value.size() < static_cast<std::size_t>(*min_length)) {
                return "plugin output field has invalid constraint: " + field + ":minLength";
            }
        }
        if (const auto max_length = JsonIntField(field_schema, "maxLength"); max_length.has_value()) {
            std::size_t value_index = *value_start;
            std::string string_value;
            if (!ParseJsonStringAt(output_json, value_index, string_value) ||
                string_value.size() > static_cast<std::size_t>(*max_length)) {
                return "plugin output field has invalid constraint: " + field + ":maxLength";
            }
        }
        if (const auto pattern = JsonStringField(field_schema, "pattern"); pattern.has_value()) {
            std::size_t value_index = *value_start;
            std::string string_value;
            try {
                if (!ParseJsonStringAt(output_json, value_index, string_value) ||
                    !std::regex_search(string_value, std::regex(*pattern))) {
                    return "plugin output field has invalid constraint: " + field + ":pattern";
                }
            } catch (const std::regex_error&) {
                return "plugin output field has invalid constraint: " + field + ":pattern";
            }
        }

        const auto number_token = JsonNumberTokenAt(output_json, *value_start);
        std::optional<double> number_value;
        if (number_token.has_value()) {
            try {
                std::size_t parsed = 0;
                const auto parsed_value = std::stod(*number_token, &parsed);
                if (parsed == number_token->size()) {
                    number_value = parsed_value;
                }
            } catch (...) {
            }
        }
        if (const auto minimum = JsonDoubleField(field_schema, "minimum"); minimum.has_value() &&
            (!number_value.has_value() || *number_value < *minimum)) {
            return "plugin output field has invalid constraint: " + field + ":minimum";
        }
        if (const auto maximum = JsonDoubleField(field_schema, "maximum"); maximum.has_value() &&
            (!number_value.has_value() || *number_value > *maximum)) {
            return "plugin output field has invalid constraint: " + field + ":maximum";
        }
        if (const auto exclusive_minimum = JsonDoubleField(field_schema, "exclusiveMinimum");
            exclusive_minimum.has_value() && (!number_value.has_value() || *number_value <= *exclusive_minimum)) {
            return "plugin output field has invalid constraint: " + field + ":exclusiveMinimum";
        }
        if (const auto exclusive_maximum = JsonDoubleField(field_schema, "exclusiveMaximum");
            exclusive_maximum.has_value() && (!number_value.has_value() || *number_value >= *exclusive_maximum)) {
            return "plugin output field has invalid constraint: " + field + ":exclusiveMaximum";
        }
        if (const auto multiple_of = JsonDoubleField(field_schema, "multipleOf"); multiple_of.has_value()) {
            if (!number_value.has_value() || *multiple_of <= 0.0) {
                return "plugin output field has invalid constraint: " + field + ":multipleOf";
            }
            const auto remainder = std::fmod(std::fabs(*number_value), *multiple_of);
            constexpr double kMultipleOfTolerance = 1e-9;
            if (remainder > kMultipleOfTolerance &&
                std::fabs(remainder - *multiple_of) > kMultipleOfTolerance) {
                return "plugin output field has invalid constraint: " + field + ":multipleOf";
            }
        }
    }
    if (const auto additional_properties = JsonBoolField(spec.output_schema_json, "additionalProperties");
        additional_properties.has_value() && !*additional_properties) {
        for (std::size_t index = 0; index < output_json.size();) {
            SkipJsonWhitespace(output_json, index);
            if (index >= output_json.size() || output_json[index] == '}') {
                break;
            }
            if (output_json[index] == '{' || output_json[index] == ',') {
                ++index;
                continue;
            }
            if (output_json[index] != '"') {
                ++index;
                continue;
            }
            std::string field;
            if (!ParseJsonStringAt(output_json, index, field)) {
                break;
            }
            if (std::find(declared_properties.begin(), declared_properties.end(), field) == declared_properties.end()) {
                return "plugin output field has invalid constraint: " + field + ":additionalProperties";
            }
            auto value_pos = FindJsonValueStart(output_json, field);
            if (!value_pos.has_value()) {
                break;
            }
            index = *value_pos;
            if (index < output_json.size() && output_json[index] == '{') {
                (void)JsonObjectRawAt(output_json, index);
            } else if (index < output_json.size() && output_json[index] == '"') {
                std::string ignored;
                (void)ParseJsonStringAt(output_json, index, ignored);
            } else {
                while (index < output_json.size() && output_json[index] != ',' && output_json[index] != '}') {
                    ++index;
                }
            }
        }
    }
    return {};
}

}  // namespace agentos
