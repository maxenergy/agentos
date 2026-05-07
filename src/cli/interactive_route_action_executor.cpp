#include "cli/interactive_route_action_executor.hpp"

#include "utils/signal_cancellation.hpp"

#include <chrono>
#include <iostream>

namespace agentos {

namespace {

std::string MakeMainRouteTaskId() {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return "main-route-" + std::to_string(value);
}

}  // namespace

TaskRunResult ExecuteMainRouteAction(const MainRouteAction& action,
                                     SkillRegistry& skill_registry,
                                     AgentRegistry& agent_registry,
                                     const std::filesystem::path& workspace,
                                     AgentLoop& loop,
                                     AuditLogger& audit_logger,
                                     const std::function<void(const TaskRunResult&)>& failure_printer) {
    const auto validation = ValidateMainRouteAction(action, skill_registry, agent_registry);
    if (!validation.valid) {
        TaskRunResult result;
        result.success = false;
        result.route_target = action.target;
        result.route_kind = action.target_kind == "agent" ? RouteTargetKind::agent : RouteTargetKind::skill;
        result.error_code = validation.error_code;
        result.error_message = validation.error_message;
        result.summary = validation.error_message;
        std::cout << "audit_log: " << audit_logger.log_path().string() << '\n';
        return result;
    }

    const auto objective = action.brief.empty() ? action.target : action.brief;
    TaskRequest task{
        .task_id = MakeMainRouteTaskId(),
        .task_type = action.target_kind == "agent" ? std::string("delegate") : action.target,
        .objective = objective,
        .workspace_path = workspace,
    };
    task.preferred_target = action.target;
    task.idempotency_key = task.task_id;
    task.inputs = action.arguments;
    if (!task.inputs.contains("objective")) {
        task.inputs["objective"] = objective;
    }
    task.inputs["main_route_action"] = action.action;
    task.inputs["main_route_target_kind"] = action.target_kind;
    task.inputs["main_route_target"] = action.target;
    task.timeout_ms = 0;
    task.allow_network = true;
    if (const auto allow = action.arguments.find("allow_high_risk");
        allow != action.arguments.end()) {
        task.allow_high_risk = allow->second == "true" || allow->second == "1" || allow->second == "yes";
    }
    if (const auto approval = action.arguments.find("approval_id");
        approval != action.arguments.end()) {
        task.approval_id = approval->second;
    }

    auto task_cancel = InstallSignalCancellation();
    const auto result = loop.run(task, std::move(task_cancel));
    if (!result.success && failure_printer) {
        failure_printer(result);
    }
    std::cout << "audit_log: " << audit_logger.log_path().string() << '\n';
    return result;
}

}  // namespace agentos
