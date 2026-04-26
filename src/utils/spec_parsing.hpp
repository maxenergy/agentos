#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace agentos {

std::vector<std::string> SplitNonEmpty(const std::string& value, char delimiter);
std::vector<std::string> SplitTsvFields(const std::string& line);
bool ParseStrictInt(const std::string& value, int& output);
bool ParseStrictSize(const std::string& value, std::size_t& output);
bool IsLikelyJsonObjectString(const std::string& value);
std::string JoinStrings(const std::vector<std::string>& values, char delimiter = ',');

}  // namespace agentos
