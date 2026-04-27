#pragma once

#include "core/models.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace agentos {

struct SchemaValidationResult {
    bool valid = true;
    std::vector<std::string> missing_required_fields;
    std::vector<std::string> invalid_type_fields;
    std::vector<std::string> invalid_constraint_fields;
    std::string error_message;
};

SchemaValidationResult ValidateRequiredInputFields(
    const SkillManifest& manifest,
    const StringMap& arguments);

bool IsParseableJsonObjectSchema(std::string_view schema_json);

std::string JsonObjectSchemaValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectRequiredFieldValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectPropertyTypeValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectStringConstraintValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectNumericConstraintValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectShapeConstraintValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectAdditionalPropertiesValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectDependentRequiredValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectNotRequiredValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectIfThenElseRequiredValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

std::string JsonObjectRequiredBranchValidationError(
    std::string_view schema_json,
    std::string_view object_json,
    std::string_view subject);

}  // namespace agentos
