#include "cli/main_route_action.hpp"

#include "core/policy/permission_model.hpp"
#include "core/registry/agent_registry.hpp"
#include "core/registry/skill_registry.hpp"
#include "core/schema/schema_validator.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <sstream>
#include <string>

namespace agentos {

namespace {

std::string TrimAsciiCopy(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string ShortenCopy(const std::string& text, const std::size_t max_chars) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "...";
}

std::optional<nlohmann::json> ParseJsonObjectCandidate(const std::string& text) {
    auto candidate = TrimAsciiCopy(text);
    if (candidate.rfind("```", 0) == 0) {
        const auto first_newline = candidate.find('\n');
        const auto closing = candidate.rfind("```");
        if (first_newline != std::string::npos && closing != std::string::npos && closing > first_newline) {
            candidate = candidate.substr(first_newline + 1, closing - first_newline - 1);
        }
    }

    auto try_parse = [](const std::string& value) -> std::optional<nlohmann::json> {
        try {
            auto parsed = nlohmann::json::parse(value);
            if (parsed.is_object()) {
                return parsed;
            }
        } catch (const nlohmann::json::exception&) {
        }
        return std::nullopt;
    };

    if (auto parsed = try_parse(candidate); parsed.has_value()) {
        return parsed;
    }

    const auto marker = candidate.find("agentos_route_action");
    if (marker == std::string::npos) {
        return std::nullopt;
    }
    const auto first = candidate.rfind('{', marker);
    const auto last = candidate.find_last_of('}');
    if (first == std::string::npos || last == std::string::npos || last <= first) {
        return std::nullopt;
    }
    return try_parse(candidate.substr(first, last - first + 1));
}

std::string JsonValueToInputString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return value.dump();
}

MainRouteActionValidation InvalidRouteAction(std::string error_code, std::string error_message) {
    return {
        .valid = false,
        .error_code = std::move(error_code),
        .error_message = std::move(error_message),
    };
}

bool BoolArgument(const StringMap& arguments, const std::string& key) {
    const auto it = arguments.find(key);
    if (it == arguments.end()) {
        return false;
    }
    return it->second == "true" || it->second == "1" || it->second == "yes";
}

bool HasApprovalArguments(const StringMap& arguments) {
    const auto approval = arguments.find("approval_id");
    return BoolArgument(arguments, "allow_high_risk") &&
           approval != arguments.end() &&
           !approval->second.empty();
}

std::string ApprovalRequiredMessage(const MainRouteAction& action) {
    const auto subject = "main-route-" + action.target_kind + ":" + action.target;
    const auto reason = action.brief.empty()
        ? "main-agent requested high-risk capability"
        : action.brief;
    std::ostringstream out;
    out << "approval required for high-risk " << action.target_kind << " " << action.target
        << ". Request approval with: agentos trust approval-request subject=" << subject
        << " reason=\"" << reason << "\" requested_by=local-user"
        << ". After an operator approves it with `agentos trust approval-approve approval=<approval_id>`, "
           "retry the same route action with arguments allow_high_risk=true and approval_id=<approval_id>.";
    return out.str();
}

}  // namespace

std::optional<MainRouteAction> ParseMainRouteAction(const std::string& text) {
    const auto parsed = ParseJsonObjectCandidate(text);
    if (!parsed.has_value() || !parsed->contains("agentos_route_action")) {
        return std::nullopt;
    }
    const auto& envelope = parsed->at("agentos_route_action");
    if (!envelope.is_object()) {
        return std::nullopt;
    }

    MainRouteAction action;
    action.action = envelope.value("action", std::string{});
    action.target_kind = envelope.value("target_kind", std::string{});
    action.target = envelope.value("target", std::string{});
    action.brief = envelope.value("brief", std::string{});
    action.mode = envelope.value("mode", std::string("sync"));
    if (action.action.empty() || action.target.empty()) {
        return std::nullopt;
    }
    if (action.target_kind.empty()) {
        action.target_kind = "skill";
    }
    if (envelope.contains("arguments") && envelope["arguments"].is_object()) {
        for (const auto& [key, value] : envelope["arguments"].items()) {
            action.arguments[key] = JsonValueToInputString(value);
        }
    }
    return action;
}

MainRouteActionValidation ValidateMainRouteAction(const MainRouteAction& action,
                                                  const SkillRegistry& skill_registry,
                                                  const AgentRegistry& agent_registry) {
    if (action.action != "call_capability") {
        return InvalidRouteAction(
            "UnsupportedRouteAction",
            "main route action is unsupported: " + action.action);
    }
    if (action.target_kind != "skill" && action.target_kind != "agent") {
        return InvalidRouteAction(
            "UnsupportedRouteTargetKind",
            "main route target_kind must be skill or agent: " + action.target_kind);
    }
    if (action.target.empty()) {
        return InvalidRouteAction("MissingRouteTarget", "main route action target is required");
    }

    if (action.target_kind == "skill") {
        const auto skill = skill_registry.find(action.target);
        if (!skill) {
            return InvalidRouteAction(
                "UnknownRouteSkill",
                "main requested an unregistered skill: " + action.target);
        }
        const auto manifest = skill->manifest();
        const auto input_validation = ValidateCapabilityInput(manifest, action.arguments);
        if (!input_validation.valid) {
            return InvalidRouteAction("InvalidRouteSkillInput", input_validation.error_message);
        }
        if (PermissionModel::requires_high_risk_approval(manifest.risk_level)) {
            if (HasApprovalArguments(action.arguments)) {
                return {};
            }
            return InvalidRouteAction("ApprovalRequired", ApprovalRequiredMessage(action));
        }
        return {};
    }

    const auto agent = agent_registry.find(action.target);
    if (!agent) {
        return InvalidRouteAction(
            "UnknownRouteAgent",
            "main requested an unregistered agent: " + action.target);
    }
    const auto profile = agent->profile();
    if (PermissionModel::requires_high_risk_approval(profile.risk_level)) {
        if (HasApprovalArguments(action.arguments)) {
            return {};
        }
        return InvalidRouteAction("ApprovalRequired", ApprovalRequiredMessage(action));
    }
    return {};
}

std::string BuildRouteActionResultPrompt(const std::string& original_prompt,
                                         const MainRouteAction& action,
                                         const TaskRunResult& result) {
    nlohmann::ordered_json payload;
    payload["original_user_request"] = original_prompt;
    payload["action"] = action.action;
    payload["target_kind"] = action.target_kind;
    payload["target"] = action.target;
    payload["success"] = result.success;
    payload["summary"] = result.summary;
    payload["route_target"] = result.route_target;
    payload["error_code"] = result.error_code;
    payload["error_message"] = result.error_message;
    if (!result.output_json.empty()) {
        payload["output_json"] = ShortenCopy(result.output_json, 6000);
    }

    std::ostringstream out;
    out << "[AGENTOS ROUTE ACTION RESULT]\n"
        << payload.dump(2) << "\n"
        << "[END AGENTOS ROUTE ACTION RESULT]\n\n"
        << "Synthesize a concise user-facing answer for the original request. "
           "Use the route result above as tool output. ";
    if (!result.success && result.error_code == "InvalidRouteSkillInput") {
        out << "The selected capability is valid but required inputs are missing or invalid; "
               "ask the user one concise clarification question naming the missing field(s). ";
    } else if (!result.success && result.error_code == "ApprovalRequired") {
        out << "The selected capability is high risk and needs explicit user approval before execution; "
               "show the approval command from the route result and explain that the user should approve it, "
               "then retry with allow_high_risk=true and approval_id=<approval_id>. ";
    } else if (!result.success) {
        out << "Explain the route failure plainly and suggest the smallest useful next step. ";
    }
    out << "Do not emit another agentos_route_action in this response.";
    return out.str();
}

}  // namespace agentos
