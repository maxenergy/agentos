#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace agentos {

std::string EscapeJson(std::string_view value);
std::string QuoteJson(std::string_view value);
std::string BoolAsJson(bool value);
std::string NumberAsJson(int value);
std::string NumberAsJson(long long value);
std::string NumberAsJson(double value);
std::string MakeJsonObject(std::initializer_list<std::pair<std::string, std::string>> fields);

}  // namespace agentos

