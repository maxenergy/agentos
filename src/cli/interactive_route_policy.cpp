#include "cli/interactive_route_policy.hpp"

namespace agentos {

void ApplyInteractiveExecutionMode(RouteDecisionExplanation& decision) {
    switch (decision.route) {
    case InteractiveRouteKind::development_agent:
    case InteractiveRouteKind::research_agent:
        decision.execution_mode = InteractiveExecutionMode::async_job;
        break;
    case InteractiveRouteKind::local_intent:
    case InteractiveRouteKind::direct_skill:
    case InteractiveRouteKind::chat_agent:
    case InteractiveRouteKind::unknown_command:
        decision.execution_mode = InteractiveExecutionMode::sync;
        break;
    }
}

InteractiveRouteVerdict MakeInteractiveRouteVerdict(
    const InteractiveRouteProposal& proposal,
    const InteractiveRouteTargets& targets) {
    RouteDecisionExplanation decision;
    decision.task_id = proposal.task_id;
    decision.user_request = proposal.user_request;
    decision.route = proposal.route;
    decision.selected_target = proposal.selected_target;
    decision.score = proposal.score;
    decision.reasons = proposal.reasons;

    if (proposal.authoritative) {
        if (decision.selected_target.empty()) {
            decision.selected_target = decision.route == InteractiveRouteKind::chat_agent
                ? targets.chat
                : "interactive_runtime";
        }
        ApplyInteractiveExecutionMode(decision);
        return {decision};
    }

    const auto& signals = proposal.signals;
    if (signals.development && (signals.workspace_mutation || signals.artifact) &&
        (!signals.research || signals.workspace_mutation)) {
        decision.route = InteractiveRouteKind::development_agent;
        decision.score += 4;
        decision.reasons.push_back("requires file/code/artifact style development work");
        decision.selected_target = targets.development;
    }

    if (signals.research) {
        if (decision.route != InteractiveRouteKind::development_agent || !signals.workspace_mutation) {
            decision.route = InteractiveRouteKind::research_agent;
            decision.selected_target = targets.research;
        }
        decision.score += 3;
        decision.reasons.push_back("requires current external research or integration analysis");
    }

    if (signals.artifact) {
        decision.score += 4;
        decision.reasons.push_back("mentions concrete deliverables or artifacts");
        if (decision.route == InteractiveRouteKind::chat_agent ||
            (decision.route == InteractiveRouteKind::research_agent && signals.workspace_mutation)) {
            decision.route = InteractiveRouteKind::development_agent;
            decision.selected_target = targets.development;
        }
    }

    if (signals.multi_step_analysis) {
        decision.score += 2;
        decision.reasons.push_back("requires multi-step analysis");
    }

    if (decision.reasons.empty()) {
        decision.route = InteractiveRouteKind::chat_agent;
        decision.selected_target = targets.chat;
        decision.reasons.push_back("no direct skill, development, or research trigger matched");
    }

    if (decision.selected_target.empty()) {
        decision.selected_target = decision.route == InteractiveRouteKind::chat_agent
            ? targets.chat
            : "unavailable";
    }

    ApplyInteractiveExecutionMode(decision);
    return {decision};
}

}  // namespace agentos
