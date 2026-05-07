#pragma once

#include "cli/main_route_action.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace agentos {

struct ChatTranscriptTurn {
    std::string user;
    std::string assistant;
};

struct PendingRouteAction {
    bool active = false;
    MainRouteAction action;
    std::string error_code;
    std::string error_message;
};

enum class ContextPrivacyLevel {
    digest,
    none,
    verbatim,
};

std::string ContextPrivacyLevelName(ContextPrivacyLevel level);

ContextPrivacyLevel ParseContextPrivacyLevel(std::string_view value);

std::string RenderRecentChatContext(const std::vector<ChatTranscriptTurn>& history,
                                    ContextPrivacyLevel privacy = ContextPrivacyLevel::digest);

void AppendChatTranscript(std::vector<ChatTranscriptTurn>& history,
                          std::string user,
                          std::string assistant);

std::vector<ChatTranscriptTurn> LoadChatTranscript(const std::filesystem::path& path);

void SaveChatTranscript(const std::filesystem::path& path,
                        const std::vector<ChatTranscriptTurn>& history);

std::string RenderPendingRouteActionContext(const PendingRouteAction& pending);

}  // namespace agentos
