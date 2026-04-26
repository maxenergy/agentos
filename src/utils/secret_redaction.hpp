#pragma once

#include "core/models.hpp"

#include <string>
#include <vector>

namespace agentos {

bool IsSensitiveFieldName(const std::string& name);
std::vector<std::string> SensitiveValuesFromMap(const StringMap& values);
std::string RedactSensitiveText(std::string text, const std::vector<std::string>& sensitive_values);
std::string RedactSensitiveText(std::string text, const StringMap& values);

}  // namespace agentos
