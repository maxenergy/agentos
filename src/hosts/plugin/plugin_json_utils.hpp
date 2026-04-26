#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

void SkipJsonWhitespace(const std::string& text, std::size_t& pos);
std::optional<std::size_t> FindJsonValueStart(const std::string& text, const std::string& key);
bool ParseJsonStringAt(const std::string& text, std::size_t& pos, std::string& output);
std::optional<std::string> JsonStringField(const std::string& text, const std::string& key);
std::optional<std::vector<std::string>> JsonStringArrayField(const std::string& text, const std::string& key);
std::optional<std::vector<std::string>> JsonObjectArrayField(const std::string& text, const std::string& key);
std::optional<std::string> JsonNumberTokenField(const std::string& text, const std::string& key);
std::optional<std::string> JsonNumberTokenAt(const std::string& text, std::size_t pos);
std::optional<int> JsonIntField(const std::string& text, const std::string& key);
std::optional<double> JsonDoubleField(const std::string& text, const std::string& key);
std::optional<bool> JsonBoolField(const std::string& text, const std::string& key);
std::optional<std::string> JsonObjectRawField(const std::string& text, const std::string& key);
std::optional<std::string> JsonObjectRawAt(const std::string& text, std::size_t& pos);
std::optional<std::string> JsonSchemaField(const std::string& text, const std::string& key);

}  // namespace agentos
