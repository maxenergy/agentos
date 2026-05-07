#pragma once

#include "cli/interactive_chat_state.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

std::filesystem::path MainRoutingTracePath(const std::filesystem::path& workspace);

void AppendMainRoutingTrace(const std::filesystem::path& workspace,
                            nlohmann::ordered_json event);

void ClearMainRoutingTrace(const std::filesystem::path& workspace);

std::vector<std::string> TailTextFile(const std::filesystem::path& path,
                                      std::size_t max_lines);

std::optional<std::size_t> ParsePositiveSize(const std::string& value);

std::string FormatRoutingTraceLine(const std::string& line);

void PrintMainContextSummary(const std::filesystem::path& path,
                             const std::string& session_name,
                             const std::vector<ChatTranscriptTurn>& history);

bool IsValidContextName(const std::string& name);

std::filesystem::path MainContextSessionsDir(const std::filesystem::path& workspace);

std::filesystem::path MainContextCurrentPath(const std::filesystem::path& workspace);

std::filesystem::path MainContextSessionPath(const std::filesystem::path& workspace,
                                             const std::string& session_name);

std::filesystem::path MainContextPrivacyDir(const std::filesystem::path& workspace);

std::filesystem::path MainContextPrivacyPath(const std::filesystem::path& workspace,
                                             const std::string& session_name);

ContextPrivacyLevel LoadMainContextPrivacy(const std::filesystem::path& workspace,
                                           const std::string& session_name);

void SaveMainContextPrivacy(const std::filesystem::path& workspace,
                            const std::string& session_name,
                            ContextPrivacyLevel privacy);

std::string LoadCurrentMainContextName(const std::filesystem::path& workspace);

void SaveCurrentMainContextName(const std::filesystem::path& workspace,
                                const std::string& session_name);

void PrintMainContextList(const std::filesystem::path& workspace,
                          const std::string& active_session);

}  // namespace agentos
