#include "hosts/agents/qwen_agent.hpp"

#include "auth/auth_models.hpp"
#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/command_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/path_utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <sstream>

namespace agentos {

namespace {

constexpr char kQwenChatCompletionsUrl[] = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/chat/completions";
constexpr char kDefaultQwenModel[] = "qwen-plus";

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

bool StartsWithCaseInsensitive(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> JsonStringValueAt(const std::string& json, std::size_t position) {
    position = json.find('"', position);
    if (position == std::string::npos) {
        return std::nullopt;
    }
    ++position;

    std::string value;
    bool escaping = false;
    for (; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaping) {
            switch (ch) {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            case '"':
            case '\\':
            case '/':
                value.push_back(ch);
                break;
            default:
                value.push_back(ch);
                break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }

    return std::nullopt;
}

}  // namespace

QwenAgent::QwenAgent(
    const CliHost& cli_host,
    const CredentialBroker& credential_broker,
    const AuthProfileStore& profile_store,
    std::filesystem::path workspace_root)
    : cli_host_(cli_host),
      credential_broker_(credential_broker),
      profile_store_(profile_store),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))) {}

AgentProfile QwenAgent::profile() const {
    return {
        .agent_name = "qwen",
        .version = "0.1.0",
        .description = "Adapter for Alibaba Cloud Model Studio Qwen through the authenticated model provider layer.",
        .capabilities = {
            {"analysis", 85},
            {"planning", 80},
            {"code_reasoning", 85},
        },
        .supports_session = false,
        .supports_streaming = false,
        .supports_patch = false,
        .supports_subagents = false,
        .supports_network = true,
        .cost_tier = "provider-billed",
        .latency_tier = "medium",
        .risk_level = "medium",
    };
}

bool QwenAgent::healthy() const {
    const auto session = credential_broker_.get_session(AuthProviderId::qwen, profile_name());
    return session.has_value() && (CommandExists("curl") || CommandExists("curl.exe"));
}

std::string QwenAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "qwen-session-" + std::to_string(next_id);
}

void QwenAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult QwenAgent::run_task(const AgentTask& task) {
    const auto started_at = std::chrono::steady_clock::now();

    const auto workspace_path = NormalizeWorkspaceRoot(task.workspace_path.empty() ? workspace_root_ : task.workspace_path);
    if (!IsPathInsideWorkspace(workspace_root_, workspace_path)) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "WorkspaceEscapeDenied",
            .error_message = "agent workspace must stay inside the configured root",
        };
    }

    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "curl was not found on PATH",
        };
    }

    const auto profile = profile_name();
    const auto session = credential_broker_.get_session(AuthProviderId::qwen, profile);
    if (!session.has_value()) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = "qwen auth session is unavailable for profile " + profile,
        };
    }

    std::string token;
    try {
        token = credential_broker_.get_access_token(AuthProviderId::qwen, profile);
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = error.what(),
        };
    }

    const auto model = model_name(task);
    const auto body = BuildRequestBody(task);
    const CliSpec spec{
        .name = "qwen_chat_completions",
        .description = "Call Alibaba Cloud Model Studio OpenAI-compatible Chat Completions.",
        .binary = "curl",
        .args_template = {
            "-L",
            "--silent",
            "--show-error",
            "--fail-with-body",
            "--max-time",
            "{{max_time_seconds}}",
            "{{url}}",
            "-H",
            "Content-Type: application/json",
            "-H",
            "Authorization: Bearer {{api_key}}",
            "-X",
            "POST",
            "-d",
            "{{request_body}}",
        },
        .required_args = {"url", "api_key", "request_body", "max_time_seconds"},
        .input_schema_json = R"({"type":"object","required":["url","request_body"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "json",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : 120000,
        .output_limit_bytes = 1024 * 1024,
    };

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"url", kQwenChatCompletionsUrl},
            {"api_key", token},
            {"request_body", body},
            {"max_time_seconds", std::to_string(std::max(1, spec.timeout_ms / 1000))},
        },
        .workspace_path = workspace_path,
    });

    const auto extracted_text = ExtractFirstMessageContent(result.stdout_text);
    const auto summary = result.success ? (extracted_text.empty() ? result.stdout_text : extracted_text) : "";
    const auto legacy_output = MakeJsonObject({
        {"agent", QuoteJson("qwen")},
        {"provider", QuoteJson("qwen")},
        {"profile", QuoteJson(profile)},
        {"model", QuoteJson(model)},
        {"content", QuoteJson(summary)},
        {"command", QuoteJson(result.command_display)},
        {"exit_code", NumberAsJson(result.exit_code)},
        {"response", QuoteJson(result.stdout_text)},
        {"stderr", QuoteJson(result.stderr_text)},
    });
    AgentResult agent_result{
        .success = result.success,
        .summary = result.success ? summary : "Qwen chat completions request failed.",
        .structured_output_json = legacy_output,
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "qwen",
        .success = agent_result.success,
        .summary = agent_result.summary,
        .structured_output_json = legacy_output,
        .artifacts = agent_result.artifacts,
        .duration_ms = agent_result.duration_ms,
        .estimated_cost = agent_result.estimated_cost,
        .error_code = agent_result.error_code,
        .error_message = agent_result.error_message,
    });
    return agent_result;
}

AgentResult QwenAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool QwenAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

std::string QwenAgent::profile_name() const {
    return profile_store_.default_profile(AuthProviderId::qwen).value_or("default");
}

std::string QwenAgent::model_name(const AgentTask& task) {
    const auto marker = task.constraints_json.find("\"model\":\"");
    if (marker != std::string::npos) {
        const auto start = marker + 9;
        const auto end = task.constraints_json.find('"', start);
        if (end != std::string::npos && end > start) {
            return task.constraints_json.substr(start, end - start);
        }
    }
    return kDefaultQwenModel;
}

std::string QwenAgent::BuildPrompt(const AgentTask& task) {
    if (StartsWithCaseInsensitive(task.objective, "return exactly:")) {
        return task.objective;
    }

    std::ostringstream prompt;
    prompt
        << "You are running as a model provider agent inside AgentOS.\n"
        << "Return a concise, useful answer for the requested task.\n"
        << "If the objective asks for an exact phrase or exact output, return only that exact content.\n\n"
        << "Task id: " << task.task_id << "\n"
        << "Task type: " << task.task_type << "\n"
        << "Objective: " << task.objective << "\n"
        << "Workspace: " << task.workspace_path << "\n";

    if (!task.context_json.empty()) {
        prompt << "Context JSON: " << task.context_json << "\n";
    }
    if (!task.constraints_json.empty()) {
        prompt << "Constraints JSON: " << task.constraints_json << "\n";
    }

    return prompt.str();
}

std::string QwenAgent::BuildRequestBody(const AgentTask& task) {
    return MakeJsonObject({
        {"model", QuoteJson(model_name(task))},
        {"messages", "[{\"role\":\"user\",\"content\":" + QuoteJson(BuildPrompt(task)) + "}]"},
    });
}

std::string QwenAgent::ExtractFirstMessageContent(const std::string& response_json) {
    const auto message_key = response_json.find("\"message\"");
    const auto content_key = response_json.find("\"content\"", message_key == std::string::npos ? 0 : message_key);
    if (content_key == std::string::npos) {
        return "";
    }
    const auto colon = response_json.find(':', content_key);
    if (colon == std::string::npos) {
        return "";
    }
    return JsonStringValueAt(response_json, colon + 1).value_or("");
}

}  // namespace agentos
