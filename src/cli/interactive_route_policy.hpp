#pragma once

#include "cli/intent_classifier.hpp"

#include <string>
#include <vector>

namespace agentos {

struct InteractiveRouteSignals {
    bool development = false;
    bool research = false;
    bool workspace_mutation = false;
    bool artifact = false;
    bool multi_step_analysis = false;
};

struct InteractiveRouteTargets {
    std::string chat;
    std::string development;
    std::string research;
};

struct InteractiveRouteProposal {
    std::string task_id;
    std::string user_request;
    InteractiveRouteKind route = InteractiveRouteKind::chat_agent;
    InteractiveRouteSignals signals;
    std::string selected_target;
    int score = 0;
    std::vector<std::string> reasons;
    bool authoritative = false;
};

struct InteractiveRouteVerdict {
    RouteDecisionExplanation decision;
};

void ApplyInteractiveExecutionMode(RouteDecisionExplanation& decision);

InteractiveRouteVerdict MakeInteractiveRouteVerdict(
    const InteractiveRouteProposal& proposal,
    const InteractiveRouteTargets& targets);

}  // namespace agentos
