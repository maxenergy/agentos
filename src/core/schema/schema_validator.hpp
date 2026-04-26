#pragma once

#include "core/models.hpp"

#include <string>
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

}  // namespace agentos
