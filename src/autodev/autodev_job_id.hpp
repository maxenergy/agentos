#pragma once

#include <string>

namespace agentos {

std::string GenerateAutoDevJobId();
bool IsValidAutoDevJobId(const std::string& job_id);
std::string AutoDevJobIdSuffix(const std::string& job_id);

}  // namespace agentos
