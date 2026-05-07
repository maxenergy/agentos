#pragma once

#include "cli/main_route_action.hpp"

#include <string>
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

std::string RenderRecentChatContext(const std::vector<ChatTranscriptTurn>& history);

void AppendChatTranscript(std::vector<ChatTranscriptTurn>& history,
                          std::string user,
                          std::string assistant);

std::string RenderPendingRouteActionContext(const PendingRouteAction& pending);

}  // namespace agentos
