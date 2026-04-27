#include "core/schema/schema_validator.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

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

bool JsonValueMatchesSchemaType(const nlohmann::ordered_json& value, const std::string& type) {
    if (type == "string") {
        return value.is_string();
    }
    if (type == "number") {
        return value.is_number();
    }
    if (type == "integer") {
        return value.is_number_integer();
    }
    if (type == "boolean") {
        return value.is_boolean();
    }
    if (type == "object") {
        return value.is_object();
    }
    if (type == "array") {
        return value.is_array();
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

void AppendFailures(std::vector<std::string>& target, const std::vector<std::string>& failures) {
    target.insert(target.end(), failures.begin(), failures.end());
}

bool HasFieldValidationFailure(const std::vector<std::string>& failures, const std::string& field) {
    const auto prefix = field + ":";
    return std::any_of(failures.begin(), failures.end(), [&](const std::string& failure) {
        return failure.rfind(prefix, 0) == 0;
    });
}

std::optional<std::string> JsonStringField(const nlohmann::ordered_json& object, const std::string& key) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return std::nullopt;
}

std::optional<int> JsonIntField(const nlohmann::ordered_json& object, const std::string& key) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_number_integer()) {
        return found->get<int>();
    }
    return std::nullopt;
}

std::optional<double> JsonDoubleField(const nlohmann::ordered_json& object, const std::string& key) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_number()) {
        return found->get<double>();
    }
    return std::nullopt;
}

std::optional<bool> JsonBoolField(const nlohmann::ordered_json& object, const std::string& key) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_boolean()) {
        return found->get<bool>();
    }
    return std::nullopt;
}

const nlohmann::ordered_json* JsonObjectField(const nlohmann::ordered_json& object, const std::string& key) {
    const auto found = object.find(key);
    if (found != object.end() && found->is_object()) {
        return &(*found);
    }
    return nullptr;
}

std::vector<std::string> JsonObjectFieldNames(const nlohmann::ordered_json& object) {
    std::vector<std::string> fields;
    if (!object.is_object()) {
        return fields;
    }
    fields.reserve(object.size());
    for (const auto& [key, unused_value] : object.items()) {
        (void)unused_value;
        fields.push_back(key);
    }
    return fields;
}

std::vector<std::string> JsonStringArrayField(const nlohmann::ordered_json& object, const std::string& key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) {
        return {};
    }

    std::vector<std::string> values;
    for (const auto& value : *found) {
        if (value.is_string()) {
            values.push_back(value.get<std::string>());
        }
    }
    return values;
}

std::optional<nlohmann::ordered_json> ParseJsonObject(const std::string_view json) {
    try {
        auto parsed = nlohmann::ordered_json::parse(json.begin(), json.end());
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const nlohmann::json::exception&) {
    }
    return std::nullopt;
}

struct JsonObjectPair {
    nlohmann::ordered_json schema;
    nlohmann::ordered_json object;
};

std::optional<JsonObjectPair> ParseJsonObjectPair(
    const std::string_view schema_json,
    const std::string_view object_json) {
    auto schema = ParseJsonObject(schema_json);
    if (!schema.has_value()) {
        return std::nullopt;
    }
    auto object = ParseJsonObject(object_json);
    if (!object.has_value()) {
        return std::nullopt;
    }
    return JsonObjectPair{std::move(*schema), std::move(*object)};
}

template <typename Validator>
std::string JsonObjectValidationErrorForParsedPair(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject,
    Validator validator) {
    const auto parsed = ParseJsonObjectPair(schema_json, object_json);
    if (!parsed.has_value()) {
        return {};
    }
    return validator(parsed->schema, parsed->object, subject);
}

template <typename... Validators>
std::string FirstJsonObjectParsedValidationError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject,
    Validators... validators) {
    std::string error;
    ((error.empty() ? error = validators(schema, object, subject) : error), ...);
    return error;
}

std::vector<std::string> JsonObjectRequiredFields(const nlohmann::ordered_json& schema) {
    if (schema.is_object()) {
        return JsonStringArrayField(schema, "required");
    }
    return {};
}

template <typename ContainsField, typename MissingFieldCallback>
bool VisitJsonRequiredMissingFields(
    const nlohmann::ordered_json& schema,
    ContainsField contains_field,
    MissingFieldCallback on_missing_field) {
    for (const auto& field : JsonObjectRequiredFields(schema)) {
        if (!contains_field(field) && !on_missing_field(field)) {
            return false;
        }
    }
    return true;
}

std::vector<nlohmann::ordered_json> JsonObjectArrayField(
    const nlohmann::ordered_json& object,
    const std::string& key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array()) {
        return {};
    }

    std::vector<nlohmann::ordered_json> values;
    for (const auto& value : *found) {
        if (value.is_object()) {
            values.push_back(value);
        }
    }
    return values;
}

template <typename Callback>
bool VisitJsonObjectPropertySchemas(const nlohmann::ordered_json& schema, Callback callback) {
    const auto* properties = JsonObjectField(schema, "properties");
    if (properties == nullptr) {
        return true;
    }
    for (const auto& [field, field_schema] : properties->items()) {
        if (!field_schema.is_object()) {
            continue;
        }
        if (!callback(field, field_schema)) {
            return false;
        }
    }
    return true;
}

template <typename IsTypeValid, typename MismatchCallback>
bool VisitJsonPropertyTypeMismatches(
    const nlohmann::ordered_json& schema,
    IsTypeValid is_type_valid,
    MismatchCallback on_mismatch) {
    return VisitJsonObjectPropertySchemas(schema, [&](const std::string& field, const nlohmann::ordered_json& field_schema) {
        const auto type = JsonStringField(field_schema, "type");
        if (!type.has_value() || is_type_valid(field, *type)) {
            return true;
        }
        return on_mismatch(field, *type);
    });
}

template <typename Callback>
bool VisitJsonInputPropertyValues(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures,
    Callback callback) {
    if (!schema.is_object()) {
        return true;
    }
    return VisitJsonObjectPropertySchemas(schema, [&](const std::string& field, const nlohmann::ordered_json& field_schema) {
        if (HasFieldValidationFailure(type_failures, field)) {
            return true;
        }
        const auto argument = arguments.find(field);
        if (argument == arguments.end()) {
            return true;
        }
        return callback(field, field_schema, argument->second);
    });
}

template <typename Callback>
bool VisitJsonTypedObjectPropertyValues(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    Callback callback) {
    if (!schema.is_object() || !object.is_object()) {
        return true;
    }
    return VisitJsonObjectPropertySchemas(schema, [&](const std::string& field, const nlohmann::ordered_json& field_schema) {
        if (!JsonStringField(field_schema, "type").has_value()) {
            return true;
        }
        const auto value = object.find(field);
        if (value == object.end()) {
            return true;
        }
        return callback(field, field_schema, *value);
    });
}

enum class JsonStringValueConstraintOrder {
    ConstThenEnum,
    EnumThenConst,
};

template <typename FailureCallback>
bool VisitJsonStringValueConstraintFailures(
    const nlohmann::ordered_json& field_schema,
    const std::string* value,
    JsonStringValueConstraintOrder order,
    FailureCallback on_failure) {
    const auto const_value = JsonStringField(field_schema, "const");
    const auto enum_values = JsonStringArrayField(field_schema, "enum");
    if (!const_value.has_value() && enum_values.empty()) {
        return true;
    }
    if (value == nullptr) {
        return on_failure("string-value");
    }

    const auto visit_const = [&]() {
        if (const_value.has_value() && *value != *const_value && !on_failure("const")) {
            return false;
        }
        return true;
    };
    const auto visit_enum = [&]() {
        if (!enum_values.empty() &&
            std::find(enum_values.begin(), enum_values.end(), *value) == enum_values.end() &&
            !on_failure("enum")) {
            return false;
        }
        return true;
    };

    if (order == JsonStringValueConstraintOrder::ConstThenEnum) {
        return visit_const() && visit_enum();
    }
    return visit_enum() && visit_const();
}

template <typename FailureCallback>
bool VisitJsonStringLengthPatternConstraintFailures(
    const nlohmann::ordered_json& field_schema,
    const std::string* value,
    FailureCallback on_failure) {
    if (const auto min_length = JsonIntField(field_schema, "minLength");
        min_length.has_value() &&
        (value == nullptr || value->size() < static_cast<std::size_t>(*min_length)) &&
        !on_failure("minLength")) {
        return false;
    }
    if (const auto max_length = JsonIntField(field_schema, "maxLength");
        max_length.has_value() &&
        (value == nullptr || value->size() > static_cast<std::size_t>(*max_length)) &&
        !on_failure("maxLength")) {
        return false;
    }
    if (const auto pattern = JsonStringField(field_schema, "pattern"); pattern.has_value()) {
        if (value == nullptr) {
            return on_failure("pattern");
        }
        try {
            if (!std::regex_search(*value, std::regex(*pattern)) && !on_failure("pattern")) {
                return false;
            }
        } catch (const std::regex_error&) {
            if (!on_failure("pattern")) {
                return false;
            }
        }
    }
    return true;
}

template <typename FailureCallback>
bool VisitJsonNumericConstraintFailures(
    const nlohmann::ordered_json& field_schema,
    const std::optional<double>& number,
    FailureCallback on_failure) {
    if (const auto minimum = JsonDoubleField(field_schema, "minimum");
        minimum.has_value() && (!number.has_value() || *number < *minimum) &&
        !on_failure("minimum")) {
        return false;
    }
    if (const auto maximum = JsonDoubleField(field_schema, "maximum");
        maximum.has_value() && (!number.has_value() || *number > *maximum) &&
        !on_failure("maximum")) {
        return false;
    }
    if (const auto exclusive_minimum = JsonDoubleField(field_schema, "exclusiveMinimum");
        exclusive_minimum.has_value() && (!number.has_value() || *number <= *exclusive_minimum) &&
        !on_failure("exclusiveMinimum")) {
        return false;
    }
    if (const auto exclusive_maximum = JsonDoubleField(field_schema, "exclusiveMaximum");
        exclusive_maximum.has_value() && (!number.has_value() || *number >= *exclusive_maximum) &&
        !on_failure("exclusiveMaximum")) {
        return false;
    }
    if (const auto multiple_of = JsonDoubleField(field_schema, "multipleOf"); multiple_of.has_value()) {
        bool failed = !number.has_value() || *multiple_of <= 0.0;
        if (!failed) {
            const auto remainder = std::fmod(std::fabs(*number), *multiple_of);
            constexpr double kMultipleOfTolerance = 1e-9;
            failed = remainder > kMultipleOfTolerance &&
                     std::fabs(remainder - *multiple_of) > kMultipleOfTolerance;
        }
        if (failed && !on_failure("multipleOf")) {
            return false;
        }
    }
    return true;
}

template <typename ConstraintFailureCallback>
bool VisitJsonPropertyNameSchemaFailures(
    const std::string& property_name,
    const nlohmann::ordered_json& property_names_schema,
    ConstraintFailureCallback on_failure) {
    if (const auto min_length = JsonIntField(property_names_schema, "minLength");
        min_length.has_value() && property_name.size() < static_cast<std::size_t>(*min_length) &&
        !on_failure("minLength")) {
        return false;
    }
    if (const auto max_length = JsonIntField(property_names_schema, "maxLength");
        max_length.has_value() && property_name.size() > static_cast<std::size_t>(*max_length) &&
        !on_failure("maxLength")) {
        return false;
    }
    if (const auto pattern = JsonStringField(property_names_schema, "pattern"); pattern.has_value()) {
        try {
            if (!std::regex_search(property_name, std::regex(*pattern)) && !on_failure("pattern")) {
                return false;
            }
        } catch (const std::regex_error&) {
            if (!on_failure("pattern")) {
                return false;
            }
        }
    }
    if (const auto const_value = JsonStringField(property_names_schema, "const");
        const_value.has_value() && property_name != *const_value && !on_failure("const")) {
        return false;
    }
    const auto enum_values = JsonStringArrayField(property_names_schema, "enum");
    if (!enum_values.empty() &&
        std::find(enum_values.begin(), enum_values.end(), property_name) == enum_values.end() &&
        !on_failure("enum")) {
        return false;
    }
    return true;
}

bool JsonPropertyNameMatchesSchema(
    const std::string& property_name,
    const nlohmann::ordered_json& property_names_schema) {
    return VisitJsonPropertyNameSchemaFailures(
        property_name,
        property_names_schema,
        [](const std::string&) {
            return false;
        });
}

bool JsonValueMatchesArrayItemSchema(
    const nlohmann::ordered_json& item_schema,
    const nlohmann::ordered_json& item) {
    if (const auto item_type = JsonStringField(item_schema, "type");
        item_type.has_value() && !JsonValueMatchesSchemaType(item, *item_type)) {
        return false;
    }

    if (item.is_object()) {
        for (const auto& required_field : JsonObjectRequiredFields(item_schema)) {
            if (!item.contains(required_field)) {
                return false;
            }
        }
        if (!VisitJsonObjectPropertySchemas(
                item_schema,
                [&](const std::string& field, const nlohmann::ordered_json& field_schema) {
                    const auto value = item.find(field);
                    if (value == item.end()) {
                        return true;
                    }
                    return JsonValueMatchesArrayItemSchema(field_schema, *value);
                })) {
            return false;
        }
        if (const auto min_properties = JsonIntField(item_schema, "minProperties");
            min_properties.has_value() && item.size() < static_cast<std::size_t>(*min_properties)) {
            return false;
        }
        if (const auto max_properties = JsonIntField(item_schema, "maxProperties");
            max_properties.has_value() && item.size() > static_cast<std::size_t>(*max_properties)) {
            return false;
        }
        if (JsonBoolField(item_schema, "additionalProperties").has_value() &&
            !JsonBoolField(item_schema, "additionalProperties").value()) {
            std::vector<std::string> declared_properties;
            VisitJsonObjectPropertySchemas(
                item_schema,
                [&](const std::string& field, const nlohmann::ordered_json&) {
                    declared_properties.push_back(field);
                    return true;
                });
            for (const auto& [field, value] : item.items()) {
                (void)value;
                if (std::find(declared_properties.begin(), declared_properties.end(), field) ==
                    declared_properties.end()) {
                    return false;
                }
            }
        }
        if (const auto* property_names_schema = JsonObjectField(item_schema, "propertyNames");
            property_names_schema != nullptr) {
            for (const auto& [field, value] : item.items()) {
                (void)value;
                if (!JsonPropertyNameMatchesSchema(field, *property_names_schema)) {
                    return false;
                }
            }
        }
        for (const auto& keyword : {"dependentRequired", "dependencies"}) {
            const auto* dependency_object = JsonObjectField(item_schema, keyword);
            if (dependency_object == nullptr) {
                continue;
            }
            for (const auto& field : JsonObjectFieldNames(*dependency_object)) {
                if (!item.contains(field)) {
                    continue;
                }
                for (const auto& required_field : JsonStringArrayField(*dependency_object, field)) {
                    if (!item.contains(required_field)) {
                        return false;
                    }
                }
            }
        }
        if (const auto* not_schema = JsonObjectField(item_schema, "not"); not_schema != nullptr) {
            const auto required_fields = JsonObjectRequiredFields(*not_schema);
            if (!required_fields.empty() &&
                std::all_of(required_fields.begin(), required_fields.end(), [&](const std::string& field) {
                    return item.contains(field);
                })) {
                return false;
            }
        }
    }

    std::optional<std::string> item_string;
    if (item.is_string()) {
        item_string = item.get<std::string>();
    }
    const auto* item_string_ptr = item_string.has_value() ? &*item_string : nullptr;
    if (!VisitJsonStringValueConstraintFailures(
            item_schema,
            item_string_ptr,
            JsonStringValueConstraintOrder::ConstThenEnum,
            [](const std::string&) {
                return false;
            })) {
        return false;
    }
    if (!VisitJsonStringLengthPatternConstraintFailures(
            item_schema,
            item_string_ptr,
            [](const std::string&) {
                return false;
            })) {
        return false;
    }

    std::optional<double> item_number;
    if (item.is_number()) {
        item_number = item.get<double>();
    }
    return VisitJsonNumericConstraintFailures(
        item_schema,
        item_number,
        [](const std::string&) {
            return false;
        });
}

template <typename FailureCallback>
bool VisitJsonArrayConstraintFailures(
    const nlohmann::ordered_json& field_schema,
    const nlohmann::ordered_json& value,
    FailureCallback on_failure) {
    if (!value.is_array()) {
        return true;
    }
    if (const auto min_items = JsonIntField(field_schema, "minItems");
        min_items.has_value() && value.size() < static_cast<std::size_t>(*min_items) &&
        !on_failure("minItems")) {
        return false;
    }
    if (const auto max_items = JsonIntField(field_schema, "maxItems");
        max_items.has_value() && value.size() > static_cast<std::size_t>(*max_items) &&
        !on_failure("maxItems")) {
        return false;
    }
    if (JsonBoolField(field_schema, "uniqueItems").value_or(false)) {
        for (auto left = value.begin(); left != value.end(); ++left) {
            for (auto right = std::next(left); right != value.end(); ++right) {
                if (*left == *right && !on_failure("uniqueItems")) {
                    return false;
                }
            }
        }
    }
    const auto prefix_items = field_schema.find("prefixItems");
    if (prefix_items != field_schema.end() && prefix_items->is_array()) {
        std::size_t index = 0;
        for (const auto& item_schema : *prefix_items) {
            if (index >= value.size()) {
                break;
            }
            if (item_schema.is_object() && !JsonValueMatchesArrayItemSchema(item_schema, value[index]) &&
                !on_failure("prefixItems")) {
                return false;
            }
            ++index;
        }
    }
    if (const auto* contains_schema = JsonObjectField(field_schema, "contains"); contains_schema != nullptr) {
        const auto match_count = static_cast<int>(std::count_if(
            value.begin(),
            value.end(),
            [&](const nlohmann::ordered_json& item) {
                return JsonValueMatchesArrayItemSchema(*contains_schema, item);
            }));
        const auto min_contains = JsonIntField(field_schema, "minContains");
        const auto max_contains = JsonIntField(field_schema, "maxContains");
        const auto required_matches = min_contains.value_or(1);
        if (match_count < required_matches && !on_failure(min_contains.has_value() ? "minContains" : "contains")) {
            return false;
        }
        if (max_contains.has_value() && match_count > *max_contains && !on_failure("maxContains")) {
            return false;
        }
    }
    if (const auto* items_schema = JsonObjectField(field_schema, "items"); items_schema != nullptr) {
        if (const auto item_type = JsonStringField(*items_schema, "type"); item_type.has_value()) {
            const auto invalid_item =
                std::find_if(value.begin(), value.end(), [&](const nlohmann::ordered_json& item) {
                    return !JsonValueMatchesSchemaType(item, *item_type);
                });
            if (invalid_item != value.end() && !on_failure("items:type")) {
                return false;
            }
        }
        const auto item_type = JsonStringField(*items_schema, "type");
        const auto has_object_item_constraints =
            (item_type.has_value() && *item_type == "object") ||
            !JsonObjectRequiredFields(*items_schema).empty() ||
            JsonObjectField(*items_schema, "properties") != nullptr ||
            JsonObjectField(*items_schema, "dependentRequired") != nullptr ||
            JsonObjectField(*items_schema, "dependencies") != nullptr ||
            JsonObjectField(*items_schema, "not") != nullptr ||
            JsonBoolField(*items_schema, "additionalProperties").has_value() ||
            JsonIntField(*items_schema, "minProperties").has_value() ||
            JsonIntField(*items_schema, "maxProperties").has_value();
        if (has_object_item_constraints) {
            const auto invalid_object_item =
                std::find_if(value.begin(), value.end(), [&](const nlohmann::ordered_json& item) {
                    return !JsonValueMatchesArrayItemSchema(*items_schema, item);
                });
            if (invalid_object_item != value.end() && !on_failure("items:object")) {
                return false;
            }
        }
        for (const auto& item : value) {
            std::optional<std::string> item_string;
            if (item.is_string()) {
                item_string = item.get<std::string>();
            }
            const auto* item_string_ptr = item_string.has_value() ? &*item_string : nullptr;
            std::string item_constraint;
            if (!VisitJsonStringValueConstraintFailures(
                    *items_schema,
                    item_string_ptr,
                    JsonStringValueConstraintOrder::ConstThenEnum,
                    [&](const std::string& constraint) {
                        item_constraint = constraint;
                        return false;
                    }) &&
                !on_failure("items:" + item_constraint)) {
                return false;
            }
            item_constraint.clear();
            if (!VisitJsonStringLengthPatternConstraintFailures(
                    *items_schema,
                    item_string_ptr,
                    [&](const std::string& constraint) {
                        item_constraint = constraint;
                        return false;
                    }) &&
                !on_failure("items:" + item_constraint)) {
                return false;
            }
            std::optional<double> item_number;
            if (item.is_number()) {
                item_number = item.get<double>();
            }
            item_constraint.clear();
            if (!VisitJsonNumericConstraintFailures(
                    *items_schema,
                    item_number,
                    [&](const std::string& constraint) {
                        item_constraint = constraint;
                        return false;
                    }) &&
                !on_failure("items:" + item_constraint)) {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::string> JsonObjectDeclaredPropertyNames(const nlohmann::ordered_json& schema) {
    std::vector<std::string> fields;
    VisitJsonObjectPropertySchemas(schema, [&](const std::string& field, const nlohmann::ordered_json& field_schema) {
        (void)field_schema;
        fields.push_back(field);
        return true;
    });
    return fields;
}

std::vector<std::string> StringMapFieldNames(const StringMap& values) {
    std::vector<std::string> fields;
    fields.reserve(values.size());
    for (const auto& [field, value] : values) {
        (void)value;
        fields.push_back(field);
    }
    return fields;
}

template <typename FailureCallback>
bool VisitJsonAdditionalPropertyFailures(
    const nlohmann::ordered_json& schema,
    const std::vector<std::string>& actual_fields,
    FailureCallback on_failure) {
    const auto additional_properties = JsonBoolField(schema, "additionalProperties");
    if (!additional_properties.has_value() || *additional_properties) {
        return true;
    }

    const auto declared_properties = JsonObjectDeclaredPropertyNames(schema);
    for (const auto& field : actual_fields) {
        if (std::find(declared_properties.begin(), declared_properties.end(), field) ==
                declared_properties.end() &&
            !on_failure(field)) {
            return false;
        }
    }
    return true;
}

template <typename ContainsField, typename MissingFieldCallback>
bool VisitJsonDependentRequiredMissingFields(
    const nlohmann::ordered_json& schema,
    ContainsField contains_field,
    MissingFieldCallback on_missing_field) {
    for (const auto& keyword : {"dependentRequired", "dependencies"}) {
        const auto* dependency_object = JsonObjectField(schema, keyword);
        if (dependency_object == nullptr) {
            continue;
        }
        for (const auto& field : JsonObjectFieldNames(*dependency_object)) {
            if (!contains_field(field)) {
                continue;
            }
            for (const auto& required_field : JsonStringArrayField(*dependency_object, field)) {
                if (!contains_field(required_field) && !on_missing_field(field, required_field)) {
                    return false;
                }
            }
        }
    }
    return true;
}

template <typename ContainsField>
bool JsonRequiredBranchMatches(
    const std::vector<std::string>& branch,
    ContainsField contains_field) {
    return std::all_of(branch.begin(), branch.end(), [&](const std::string& field) {
        return contains_field(field);
    });
}

template <typename ContainsField>
int JsonCountMatchingRequiredBranches(
    const std::vector<nlohmann::ordered_json>& branch_objects,
    ContainsField contains_field) {
    int matches = 0;
    for (const auto& branch_object : branch_objects) {
        const auto branch = JsonStringArrayField(branch_object, "required");
        if (!branch.empty() && JsonRequiredBranchMatches(branch, contains_field)) {
            ++matches;
        }
    }
    return matches;
}

template <typename ContainsField, typename FailureCallback>
bool VisitJsonAllOfRequiredBranchFailures(
    const nlohmann::ordered_json& schema,
    ContainsField contains_field,
    FailureCallback on_failure) {
    const auto all_of_branches = JsonObjectArrayField(schema, "allOf");
    if (all_of_branches.empty()) {
        return true;
    }
    for (const auto& branch_object : all_of_branches) {
        const auto branch = JsonStringArrayField(branch_object, "required");
        if (!branch.empty() && !JsonRequiredBranchMatches(branch, contains_field) && !on_failure()) {
            return false;
        }
    }
    return true;
}

template <typename ContainsField, typename FailureCallback>
bool VisitJsonRequiredBranchCountFailures(
    const nlohmann::ordered_json& schema,
    ContainsField contains_field,
    FailureCallback on_failure) {
    const auto any_of_branches = JsonObjectArrayField(schema, "anyOf");
    if (!any_of_branches.empty() &&
        JsonCountMatchingRequiredBranches(any_of_branches, contains_field) == 0 &&
        !on_failure("anyOf")) {
        return false;
    }

    const auto one_of_branches = JsonObjectArrayField(schema, "oneOf");
    if (!one_of_branches.empty() &&
        JsonCountMatchingRequiredBranches(one_of_branches, contains_field) != 1 &&
        !on_failure("oneOf")) {
        return false;
    }
    return true;
}

template <typename ContainsField, typename MatchCallback>
bool VisitJsonNotRequiredBranchMatches(
    const nlohmann::ordered_json& schema,
    ContainsField contains_field,
    MatchCallback on_match) {
    const auto* not_schema = JsonObjectField(schema, "not");
    if (not_schema == nullptr) {
        return true;
    }
    const auto not_required_fields = JsonStringArrayField(*not_schema, "required");
    if (!not_required_fields.empty() && JsonRequiredBranchMatches(not_required_fields, contains_field)) {
        return on_match();
    }
    return true;
}

template <typename ConstraintFailureCallback>
bool VisitJsonPropertyNameConstraintFailures(
    const std::string& property_name,
    const nlohmann::ordered_json& property_names_schema,
    ConstraintFailureCallback on_failure) {
    return VisitJsonPropertyNameSchemaFailures(property_name, property_names_schema, on_failure);
}

template <typename ConstraintFailureCallback>
bool VisitJsonObjectPropertyCountFailures(
    const nlohmann::ordered_json& schema,
    const std::size_t property_count,
    ConstraintFailureCallback on_failure) {
    if (const auto min_properties = JsonIntField(schema, "minProperties");
        min_properties.has_value() && property_count < static_cast<std::size_t>(*min_properties) &&
        !on_failure("minProperties")) {
        return false;
    }
    if (const auto max_properties = JsonIntField(schema, "maxProperties");
        max_properties.has_value() && property_count > static_cast<std::size_t>(*max_properties) &&
        !on_failure("maxProperties")) {
        return false;
    }
    return true;
}

std::string JsonObjectFieldConstraintError(
    const std::string_view subject,
    const std::string& field,
    const std::string& constraint) {
    return std::string(subject) + " field has invalid constraint: " + field + ":" + constraint;
}

std::string JsonObjectRequiredFieldError(
    const std::string_view subject,
    const std::string& field) {
    return std::string(subject) + " missing required field: " + field;
}

std::string JsonObjectFieldTypeError(
    const std::string_view subject,
    const std::string& field,
    const std::string& type) {
    return std::string(subject) + " field has invalid type: " + field + ":" + type;
}

std::string JsonObjectFailedConstraintError(
    const std::string_view subject,
    const std::string& constraint) {
    return std::string(subject) + " failed " + constraint;
}

std::string JsonObjectInputFieldConstraintFailure(
    const std::string& field,
    const std::string& constraint) {
    return field + ":" + constraint;
}

std::string JsonObjectInputSchemaConstraintFailure(const std::string& constraint) {
    return "schema:" + constraint;
}

std::string JsonObjectInputSchemaFieldConstraintFailure(
    const std::string& constraint,
    const std::string& field) {
    return JsonObjectInputSchemaConstraintFailure(constraint + ":" + field);
}

template <typename VisitFailures, typename MakeFailure>
std::vector<std::string> CollectJsonObjectInputFailures(
    VisitFailures visit_failures,
    MakeFailure make_failure) {
    std::vector<std::string> failures;
    visit_failures([&](const auto&... values) {
        failures.push_back(make_failure(values...));
        return true;
    });
    return failures;
}

template <typename VisitFailures, typename MakeError>
std::string FirstJsonObjectValidationError(
    VisitFailures visit_failures,
    MakeError make_error) {
    std::string error;
    visit_failures([&](const auto&... values) {
        error = make_error(values...);
        return false;
    });
    return error;
}

template <typename VisitConstraints>
std::vector<std::string> CollectJsonObjectInputPropertyConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures,
    VisitConstraints visit_constraints) {
    std::vector<std::string> failures;
    if (!schema.is_object()) {
        return failures;
    }
    VisitJsonInputPropertyValues(schema, arguments, type_failures, [&](const std::string& field,
                                                                       const nlohmann::ordered_json& field_schema,
                                                                       const std::string& value) {
        visit_constraints(
            field_schema,
            value,
            [&](const std::string& constraint) {
                failures.push_back(JsonObjectInputFieldConstraintFailure(field, constraint));
                return true;
            });
        return true;
    });
    return failures;
}

template <typename VisitConstraints>
std::string FirstJsonObjectPropertyConstraintError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject,
    VisitConstraints visit_constraints) {
    std::string error;
    VisitJsonTypedObjectPropertyValues(schema, object, [&](const std::string& field,
                                                           const nlohmann::ordered_json& field_schema,
                                                           const nlohmann::ordered_json& value) {
        return visit_constraints(
                   field_schema,
                   value,
                   [&](const std::string& constraint) {
                       error = JsonObjectFieldConstraintError(subject, field, constraint);
                       return false;
                   }) &&
               error.empty();
    });
    return error;
}

std::vector<std::string> CollectJsonObjectInputPropertyNameConstraintFailures(
    const std::vector<std::string>& property_names,
    const nlohmann::ordered_json& property_names_schema) {
    std::vector<std::string> failures;
    for (const auto& property_name : property_names) {
        AppendFailures(
            failures,
            CollectJsonObjectInputFailures(
                [&](auto on_failure) {
                    return VisitJsonPropertyNameConstraintFailures(property_name, property_names_schema, on_failure);
                },
                [&](const std::string& constraint) {
                    return JsonObjectInputSchemaFieldConstraintFailure(
                        "propertyNames:" + constraint,
                        property_name);
                }));
    }
    return failures;
}

std::string FirstJsonObjectPropertyNameConstraintError(
    const std::vector<std::string>& property_names,
    const nlohmann::ordered_json& property_names_schema,
    const std::string_view subject) {
    for (const auto& property_name : property_names) {
        if (const auto error = FirstJsonObjectValidationError(
                [&](auto on_failure) {
                    return VisitJsonPropertyNameConstraintFailures(property_name, property_names_schema, on_failure);
                },
                [&](const std::string& constraint) {
                    return JsonObjectFieldConstraintError(subject, property_name, "propertyNames:" + constraint);
                });
            !error.empty()) {
            return error;
        }
    }
    return {};
}

std::vector<std::string> CollectJsonObjectInputRequiredFieldFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonRequiredMissingFields(
                schema,
                [&](const std::string& field) {
                    return arguments.contains(field);
                },
                on_failure);
        },
        [](const std::string& field) {
            return field;
        });
}

std::vector<std::string> JsonObjectInputRequiredFieldFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    return CollectJsonObjectInputRequiredFieldFailures(schema, arguments);
}

std::string FirstJsonObjectRequiredFieldError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonRequiredMissingFields(
                schema,
                [&](const std::string& field) {
                    return object.contains(field);
                },
                on_failure);
        },
        [&](const std::string& field) {
            return JsonObjectRequiredFieldError(subject, field);
        });
}

std::string FirstJsonObjectPropertyTypeError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonPropertyTypeMismatches(
                schema,
                [&](const std::string& field, const std::string& type) {
                    const auto value = object.find(field);
                    if (value == object.end()) {
                        return true;
                    }
                    return JsonValueMatchesSchemaType(*value, type);
                },
                on_failure);
        },
        [&](const std::string& field, const std::string& type) {
            return JsonObjectFieldTypeError(subject, field, type);
        });
}

std::vector<std::string> CollectJsonObjectInputPropertyTypeFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonPropertyTypeMismatches(
                schema,
                [&](const std::string& field, const std::string& type) {
                    const auto argument = arguments.find(field);
                    if (argument == arguments.end()) {
                        return true;
                    }
                    return ArgumentMatchesSchemaType(argument->second, type);
                },
                on_failure);
        },
        [](const std::string& field, const std::string& type) {
            return JsonObjectInputFieldConstraintFailure(field, type);
        });
}

std::vector<std::string> CollectJsonObjectInputPropertyCountFailures(
    const nlohmann::ordered_json& schema,
    const std::size_t property_count) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonObjectPropertyCountFailures(schema, property_count, on_failure);
        },
        [](const std::string& constraint) {
            return JsonObjectInputSchemaConstraintFailure(constraint);
        });
}

std::string FirstJsonObjectPropertyCountError(
    const nlohmann::ordered_json& schema,
    const std::size_t property_count,
    const std::string_view subject) {
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonObjectPropertyCountFailures(schema, property_count, on_failure);
        },
        [&](const std::string& constraint) {
            return JsonObjectFailedConstraintError(subject, constraint);
        });
}

std::vector<std::string> CollectJsonObjectInputDependentRequiredFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonDependentRequiredMissingFields(
                schema,
                [&](const std::string& field) {
                    return arguments.contains(field);
                },
                on_failure);
        },
        [&](const std::string& field, const std::string& required_field) {
            return JsonObjectInputFieldConstraintFailure(
                field,
                "dependentRequired:" + required_field);
        });
}

std::string FirstJsonObjectDependentRequiredError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    const auto contains_output_field = [&](const std::string& field) {
        return object.contains(field);
    };
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonDependentRequiredMissingFields(schema, contains_output_field, on_failure);
        },
        [&](const std::string& field, const std::string& required_field) {
            return JsonObjectFieldConstraintError(subject, field, "dependentRequired:" + required_field);
        });
}

std::vector<std::string> CollectJsonObjectInputAdditionalPropertyFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonAdditionalPropertyFailures(schema, StringMapFieldNames(arguments), on_failure);
        },
        [](const std::string& field) {
            return JsonObjectInputFieldConstraintFailure(field, "additionalProperties");
        });
}

std::string FirstJsonObjectAdditionalPropertyError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonAdditionalPropertyFailures(schema, JsonObjectFieldNames(object), on_failure);
        },
        [&](const std::string& field) {
            return JsonObjectFieldConstraintError(subject, field, "additionalProperties");
        });
}

std::vector<std::string> CollectJsonObjectInputNotRequiredFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonNotRequiredBranchMatches(
                schema,
                [&](const std::string& field) {
                    return arguments.contains(field);
                },
                on_failure);
        },
        []() {
            return JsonObjectInputSchemaConstraintFailure("not:required");
        });
}

std::string FirstJsonObjectNotRequiredError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonNotRequiredBranchMatches(
                schema,
                [&](const std::string& field) {
                    return object.contains(field);
                },
                on_failure);
        },
        [&]() {
            return JsonObjectFailedConstraintError(subject, "not.required");
        });
}

std::vector<std::string> JsonObjectInputShapeConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    std::vector<std::string> failures;
    if (!schema.is_object()) {
        return failures;
    }

    if (const auto* property_names_schema = JsonObjectField(schema, "propertyNames");
        property_names_schema != nullptr) {
        AppendFailures(
            failures,
            CollectJsonObjectInputPropertyNameConstraintFailures(
                StringMapFieldNames(arguments),
                *property_names_schema));
    }
    AppendFailures(failures, CollectJsonObjectInputPropertyCountFailures(schema, arguments.size()));
    return failures;
}

std::vector<std::string> JsonObjectInputDependentRequiredConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    return CollectJsonObjectInputDependentRequiredFailures(schema, arguments);
}

std::optional<std::pair<std::string, std::string>> JsonObjectIfPropertyConstCondition(
    const nlohmann::ordered_json& schema);

template <typename StringFieldValue, typename ContainsField, typename MissingFieldCallback>
bool VisitJsonIfThenElseRequiredMissingFields(
    const nlohmann::ordered_json& schema,
    StringFieldValue string_field_value,
    ContainsField contains_field,
    MissingFieldCallback on_missing_field) {
    const auto if_condition = JsonObjectIfPropertyConstCondition(schema);
    if (!if_condition.has_value()) {
        return true;
    }
    const auto condition_value = string_field_value(if_condition->first);
    const bool condition_matched = condition_value.has_value() && *condition_value == if_condition->second;
    const auto branch_name = condition_matched ? "then" : "else";
    const auto* branch_schema = JsonObjectField(schema, branch_name);
    if (branch_schema == nullptr) {
        return true;
    }
    for (const auto& required_field : JsonStringArrayField(*branch_schema, "required")) {
        if (!contains_field(required_field) && !on_missing_field(branch_name, required_field)) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> CollectJsonObjectInputIfThenElseRequiredFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonIfThenElseRequiredMissingFields(
                schema,
                [&](const std::string& field) -> std::optional<std::string> {
                    const auto found = arguments.find(field);
                    if (found == arguments.end()) {
                        return std::nullopt;
                    }
                    return found->second;
                },
                [&](const std::string& field) {
                    return arguments.contains(field);
                },
                on_failure);
        },
        [](const std::string& branch_name, const std::string& required_field) {
            return JsonObjectInputSchemaFieldConstraintFailure(
                branch_name + ":required",
                required_field);
        });
}

std::string FirstJsonObjectIfThenElseRequiredError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonIfThenElseRequiredMissingFields(
                schema,
                [&](const std::string& field) {
                    return JsonStringField(object, field);
                },
                [&](const std::string& field) {
                    return object.contains(field);
                },
                on_failure);
        },
        [&](const std::string& branch_name, const std::string& required_field) {
            return JsonObjectFailedConstraintError(subject, branch_name + " required field: " + required_field);
        });
}

std::vector<std::string> CollectJsonObjectInputRequiredBranchCountFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    if (!schema.is_object()) {
        return {};
    }
    const auto contains_argument = [&](const std::string& field) {
        return arguments.contains(field);
    };
    return CollectJsonObjectInputFailures(
        [&](auto on_failure) {
            return VisitJsonRequiredBranchCountFailures(schema, contains_argument, on_failure);
        },
        [](const std::string& branch_keyword) {
            return JsonObjectInputSchemaConstraintFailure(branch_keyword + ":required");
        });
}

std::vector<std::string> CollectJsonObjectInputAllOfRequiredBranchFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    std::vector<std::string> failures;
    if (!schema.is_object()) {
        return failures;
    }
    const auto contains_argument = [&](const std::string& field) {
        return arguments.contains(field);
    };
    VisitJsonAllOfRequiredBranchFailures(
        schema,
        contains_argument,
        [&]() {
            failures.push_back(JsonObjectInputSchemaConstraintFailure("allOf:required"));
            return false;
        });
    return failures;
}

std::string FirstJsonObjectAllOfRequiredBranchError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    const auto contains_output_field = [&](const std::string& field) {
        return object.contains(field);
    };
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonAllOfRequiredBranchFailures(schema, contains_output_field, on_failure);
        },
        [&]() {
            return JsonObjectFailedConstraintError(subject, "allOf required branches");
        });
}

std::string FirstJsonObjectRequiredBranchCountError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    const auto contains_output_field = [&](const std::string& field) {
        return object.contains(field);
    };
    return FirstJsonObjectValidationError(
        [&](auto on_failure) {
            return VisitJsonRequiredBranchCountFailures(schema, contains_output_field, on_failure);
        },
        [&](const std::string& branch_keyword) {
            return JsonObjectFailedConstraintError(subject, branch_keyword + " required branches");
        });
}

std::vector<std::string> JsonObjectInputNotRequiredConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    return CollectJsonObjectInputNotRequiredFailures(schema, arguments);
}

std::vector<std::string> JsonObjectInputIfThenElseRequiredConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    return CollectJsonObjectInputIfThenElseRequiredFailures(schema, arguments);
}

std::vector<std::string> JsonObjectInputRequiredBranchConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    std::vector<std::string> failures;
    AppendFailures(failures, CollectJsonObjectInputRequiredBranchCountFailures(schema, arguments));
    AppendFailures(failures, CollectJsonObjectInputAllOfRequiredBranchFailures(schema, arguments));
    return failures;
}

std::vector<std::string> JsonObjectInputAdditionalPropertiesConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    return CollectJsonObjectInputAdditionalPropertyFailures(schema, arguments);
}

std::vector<std::string> JsonObjectInputPropertyTypeFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments) {
    return CollectJsonObjectInputPropertyTypeFailures(schema, arguments);
}

std::vector<std::string> CollectJsonObjectInputStringConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures) {
    return CollectJsonObjectInputPropertyConstraintFailures(
        schema,
        arguments,
        type_failures,
        [](const nlohmann::ordered_json& field_schema,
           const std::string& value,
           auto on_failure) {
            VisitJsonStringValueConstraintFailures(
                field_schema,
                &value,
                JsonStringValueConstraintOrder::EnumThenConst,
                on_failure);
            VisitJsonStringLengthPatternConstraintFailures(field_schema, &value, on_failure);
            return true;
        });
}

std::string FirstJsonObjectStringConstraintError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectPropertyConstraintError(
        schema,
        object,
        subject,
        [](const nlohmann::ordered_json& field_schema,
           const nlohmann::ordered_json& value,
           auto on_failure) {
            std::optional<std::string> string_value;
            if (value.is_string()) {
                string_value = value.get<std::string>();
            }
            const auto* string_value_ptr = string_value.has_value() ? &*string_value : nullptr;
            if (!VisitJsonStringValueConstraintFailures(
                    field_schema,
                    string_value_ptr,
                    JsonStringValueConstraintOrder::ConstThenEnum,
                    on_failure)) {
                return false;
            }
            return VisitJsonStringLengthPatternConstraintFailures(field_schema, string_value_ptr, on_failure);
        });
}

std::vector<std::string> CollectJsonObjectInputNumericConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures) {
    return CollectJsonObjectInputPropertyConstraintFailures(
        schema,
        arguments,
        type_failures,
        [](const nlohmann::ordered_json& field_schema,
           const std::string& value,
           auto on_failure) {
            return VisitJsonNumericConstraintFailures(field_schema, ParseArgumentNumber(value), on_failure);
        });
}

std::string FirstJsonObjectNumericConstraintError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectPropertyConstraintError(
        schema,
        object,
        subject,
        [](const nlohmann::ordered_json& field_schema,
           const nlohmann::ordered_json& value,
           auto on_failure) {
            std::optional<double> number_value;
            if (value.is_number()) {
                number_value = value.get<double>();
            }
            return VisitJsonNumericConstraintFailures(field_schema, number_value, on_failure);
        });
}

std::string FirstJsonObjectArrayConstraintError(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectPropertyConstraintError(
        schema,
        object,
        subject,
        [](const nlohmann::ordered_json& field_schema,
           const nlohmann::ordered_json& value,
           auto on_failure) {
            return VisitJsonArrayConstraintFailures(field_schema, value, on_failure);
        });
}

std::vector<std::string> JsonObjectInputStringConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures) {
    return CollectJsonObjectInputStringConstraintFailures(schema, arguments, type_failures);
}

std::vector<std::string> JsonObjectInputNumericConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures) {
    return CollectJsonObjectInputNumericConstraintFailures(schema, arguments, type_failures);
}

std::vector<std::string> JsonObjectInputConstraintFailures(
    const nlohmann::ordered_json& schema,
    const StringMap& arguments,
    const std::vector<std::string>& type_failures) {
    std::vector<std::string> failures;
    AppendFailures(failures, JsonObjectInputShapeConstraintFailures(schema, arguments));
    AppendFailures(failures, JsonObjectInputDependentRequiredConstraintFailures(schema, arguments));
    AppendFailures(failures, JsonObjectInputNotRequiredConstraintFailures(schema, arguments));
    AppendFailures(failures, JsonObjectInputIfThenElseRequiredConstraintFailures(schema, arguments));
    AppendFailures(failures, JsonObjectInputRequiredBranchConstraintFailures(schema, arguments));
    AppendFailures(failures, JsonObjectInputAdditionalPropertiesConstraintFailures(schema, arguments));
    AppendFailures(failures, JsonObjectInputStringConstraintFailures(schema, arguments, type_failures));
    AppendFailures(failures, JsonObjectInputNumericConstraintFailures(schema, arguments, type_failures));
    return failures;
}

std::optional<std::pair<std::string, std::string>> JsonObjectIfPropertyConstCondition(
    const nlohmann::ordered_json& schema) {
    const auto* if_schema = JsonObjectField(schema, "if");
    if (if_schema == nullptr) {
        return std::nullopt;
    }
    const auto* if_properties = JsonObjectField(*if_schema, "properties");
    if (if_properties == nullptr) {
        return std::nullopt;
    }

    for (const auto& field : JsonObjectFieldNames(*if_properties)) {
        const auto* property_schema = JsonObjectField(*if_properties, field);
        if (property_schema == nullptr) {
            continue;
        }
        if (const auto const_value = JsonStringField(*property_schema, "const"); const_value.has_value()) {
            return std::make_pair(field, *const_value);
        }
    }
    return std::nullopt;
}

std::string JsonObjectRequiredFieldValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectRequiredFieldError(schema, object, subject);
}

std::string JsonObjectPropertyTypeValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectPropertyTypeError(schema, object, subject);
}

std::string JsonObjectStringConstraintValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectStringConstraintError(schema, object, subject);
}

std::string JsonObjectNumericConstraintValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectNumericConstraintError(schema, object, subject);
}

std::string JsonObjectArrayConstraintValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectArrayConstraintError(schema, object, subject);
}

std::string JsonObjectShapeConstraintValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    const auto object_fields = JsonObjectFieldNames(object);
    if (const auto property_count_error = FirstJsonObjectPropertyCountError(schema, object_fields.size(), subject);
        !property_count_error.empty()) {
        return property_count_error;
    }
    if (const auto* property_names_schema = JsonObjectField(schema, "propertyNames");
        property_names_schema != nullptr) {
        if (const auto property_name_error =
                FirstJsonObjectPropertyNameConstraintError(object_fields, *property_names_schema, subject);
            !property_name_error.empty()) {
            return property_name_error;
        }
    }
    return {};
}

std::string JsonObjectAdditionalPropertiesValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectAdditionalPropertyError(schema, object, subject);
}

std::string JsonObjectDependentRequiredValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectDependentRequiredError(schema, object, subject);
}

std::string JsonObjectNotRequiredValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectNotRequiredError(schema, object, subject);
}

std::string JsonObjectIfThenElseRequiredValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectIfThenElseRequiredError(schema, object, subject);
}

std::string JsonObjectRequiredBranchValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    if (const auto all_of_error = FirstJsonObjectAllOfRequiredBranchError(schema, object, subject);
        !all_of_error.empty()) {
        return all_of_error;
    }

    return FirstJsonObjectRequiredBranchCountError(schema, object, subject);
}

std::string JsonObjectSchemaConstraintValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectParsedValidationError(
        schema,
        object,
        subject,
        JsonObjectStringConstraintValidationErrorForParsed,
        JsonObjectNumericConstraintValidationErrorForParsed,
        JsonObjectArrayConstraintValidationErrorForParsed,
        JsonObjectShapeConstraintValidationErrorForParsed,
        JsonObjectDependentRequiredValidationErrorForParsed,
        JsonObjectNotRequiredValidationErrorForParsed,
        JsonObjectIfThenElseRequiredValidationErrorForParsed,
        JsonObjectRequiredBranchValidationErrorForParsed,
        JsonObjectAdditionalPropertiesValidationErrorForParsed);
}

std::string JsonObjectSchemaValidationErrorForParsed(
    const nlohmann::ordered_json& schema,
    const nlohmann::ordered_json& object,
    const std::string_view subject) {
    return FirstJsonObjectParsedValidationError(
        schema,
        object,
        subject,
        JsonObjectRequiredFieldValidationErrorForParsed,
        JsonObjectPropertyTypeValidationErrorForParsed,
        JsonObjectSchemaConstraintValidationErrorForParsed);
}

}  // namespace

bool IsParseableJsonObjectSchema(const std::string_view schema_json) {
    return ParseJsonObject(schema_json).has_value();
}

std::string JsonObjectSchemaValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectSchemaValidationErrorForParsed);
}

std::string JsonObjectRequiredFieldValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectRequiredFieldValidationErrorForParsed);
}

std::string JsonObjectPropertyTypeValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectPropertyTypeValidationErrorForParsed);
}

std::string JsonObjectStringConstraintValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectStringConstraintValidationErrorForParsed);
}

std::string JsonObjectNumericConstraintValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectNumericConstraintValidationErrorForParsed);
}

std::string JsonObjectShapeConstraintValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectShapeConstraintValidationErrorForParsed);
}

std::string JsonObjectAdditionalPropertiesValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectAdditionalPropertiesValidationErrorForParsed);
}

std::string JsonObjectDependentRequiredValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectDependentRequiredValidationErrorForParsed);
}

std::string JsonObjectNotRequiredValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectNotRequiredValidationErrorForParsed);
}

std::string JsonObjectIfThenElseRequiredValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectIfThenElseRequiredValidationErrorForParsed);
}

std::string JsonObjectRequiredBranchValidationError(
    const std::string_view schema_json,
    const std::string_view object_json,
    const std::string_view subject) {
    return JsonObjectValidationErrorForParsedPair(
        schema_json, object_json, subject, JsonObjectRequiredBranchValidationErrorForParsed);
}

SchemaValidationResult ValidateRequiredInputFields(
    const SkillManifest& manifest,
    const StringMap& arguments) {
    SchemaValidationResult result;
    const auto input_schema = ParseJsonObject(manifest.input_schema_json);
    if (!input_schema.has_value()) {
        return result;
    }

    result.missing_required_fields = JsonObjectInputRequiredFieldFailures(*input_schema, arguments);

    for (const auto& failure : JsonObjectInputPropertyTypeFailures(*input_schema, arguments)) {
        result.invalid_type_fields.push_back(failure);
    }

    for (const auto& failure : JsonObjectInputConstraintFailures(
             *input_schema,
             arguments,
             result.invalid_type_fields)) {
        result.invalid_constraint_fields.push_back(failure);
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
