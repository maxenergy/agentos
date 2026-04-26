#include "hosts/agents/gemini_agent.hpp"

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

constexpr char kGeminiGenerateContentBaseUrl[] = "https://generativelanguage.googleapis.com/v1beta/models/";
constexpr char kDefaultGeminiModel[] = "gemini-2.5-flash";

std::string NormalizeGeminiModelName(const std::string& model) {
    if (model == "gemini-3.1-pro" || model == "gemini-3.1-pro-latest") {
        return "gemini-3.1-pro-preview";
    }
    if (model == "gemini-3-pro") {
        return "gemini-3-pro-preview";
    }
    return model;
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

std::string TrimWhitespace(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
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

GeminiAgent::GeminiAgent(
    const CliHost& cli_host,
    const CredentialBroker& credential_broker,
    const AuthProfileStore& profile_store,
    std::filesystem::path workspace_root)
    : cli_host_(cli_host),
      credential_broker_(credential_broker),
      profile_store_(profile_store),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))) {}

AgentProfile GeminiAgent::profile() const {
    return {
        .agent_name = "gemini",
        .version = "0.1.0",
        .description = "Adapter for Google Gemini generateContent through the authenticated model provider layer.",
        .capabilities = {
            {"analysis", 90},
            {"planning", 85},
            {"code_reasoning", 75},
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

bool GeminiAgent::healthy() const {
    const auto session = credential_broker_.get_session(AuthProviderId::gemini, profile_name());
    if (!session.has_value()) {
        return false;
    }
    if (session->mode == AuthMode::cloud_adc || session->access_token_ref == "external-cli:gcloud-adc") {
        return (CommandExists("gcloud") || CommandExists("gcloud.cmd")) &&
               (CommandExists("curl") || CommandExists("curl.exe"));
    }
    if (session->managed_by_external_cli || session->access_token_ref == "external-cli:gemini") {
        return CommandExists("gemini") || CommandExists("gemini.cmd");
    }
    return CommandExists("curl") || CommandExists("curl.exe");
}

std::string GeminiAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "gemini-session-" + std::to_string(next_id);
}

void GeminiAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult GeminiAgent::run_task(const AgentTask& task) {
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

    const auto profile = profile_name();
    const auto session = credential_broker_.get_session(AuthProviderId::gemini, profile);
    if (!session.has_value()) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AuthUnavailable",
            .error_message = "gemini auth session is unavailable for profile " + profile,
        };
    }

    if (session->mode == AuthMode::browser_oauth || session->access_token_ref == "external-cli:gemini") {
        return run_task_with_cli_session(task, *session, workspace_path);
    }

    return run_task_with_rest_session(task, *session, workspace_path);
}

AgentResult GeminiAgent::run_task_with_rest_session(
    const AgentTask& task,
    const AuthSession& session,
    const std::filesystem::path& workspace_path) {
    const auto started_at = std::chrono::steady_clock::now();

    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "curl was not found on PATH",
        };
    }

    const auto profile = profile_name();
    std::string token;
    if (session.mode == AuthMode::cloud_adc || session.access_token_ref == "external-cli:gcloud-adc") {
        if (!CommandExists("gcloud") && !CommandExists("gcloud.cmd")) {
            return {
                .success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "AuthUnavailable",
                .error_message = "gcloud was not found on PATH for Google ADC token minting",
            };
        }

        const auto token_result = cli_host_.run(CliRunRequest{
            .spec = CliSpec{
                .name = "google_adc_access_token",
                .description = "Mint a Google Application Default Credentials access token.",
                .binary = "gcloud",
                .args_template = {"auth", "application-default", "print-access-token"},
                .parse_mode = "text",
                .risk_level = "low",
                .permissions = {"process.spawn"},
                .timeout_ms = 30000,
                .output_limit_bytes = 16 * 1024,
                .env_allowlist = {
                    "USERPROFILE",
                    "HOMEDRIVE",
                    "HOMEPATH",
                    "HOME",
                    "APPDATA",
                    "LOCALAPPDATA",
                    "XDG_CONFIG_HOME",
                    "CLOUDSDK_CONFIG",
                    "GOOGLE_APPLICATION_CREDENTIALS",
                },
            },
            .workspace_path = workspace_path,
        });
        token = TrimWhitespace(token_result.stdout_text);
        if (!token_result.success || token.empty()) {
            return {
                .success = false,
                .duration_ms = token_result.duration_ms,
                .error_code = token_result.error_code.empty() ? "AuthUnavailable" : token_result.error_code,
                .error_message = token_result.error_message.empty()
                    ? "gcloud did not return a Google ADC access token"
                    : token_result.error_message,
            };
        }
    } else {
        try {
            token = credential_broker_.get_access_token(AuthProviderId::gemini, profile);
        } catch (const std::exception& error) {
            return {
                .success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "AuthUnavailable",
                .error_message = error.what(),
            };
        }
    }

    const auto model = model_name(task);
    const auto url = std::string(kGeminiGenerateContentBaseUrl) + model + ":generateContent";
    const auto auth_header = session.mode == AuthMode::api_key
        ? "x-goog-api-key: " + token
        : "Authorization: Bearer " + token;
    const auto body = BuildRequestBody(task);

    const CliSpec spec{
        .name = "gemini_generate_content",
        .description = "Call the Gemini generateContent REST endpoint.",
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
            "{{auth_header}}",
            "-X",
            "POST",
            "-d",
            "{{request_body}}",
        },
        .required_args = {"url", "auth_header", "request_body", "max_time_seconds"},
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
            {"url", url},
            {"auth_header", auth_header},
            {"api_key", token},
            {"request_body", body},
            {"max_time_seconds", std::to_string(std::max(1, spec.timeout_ms / 1000))},
        },
        .workspace_path = workspace_path,
    });

    const auto extracted_text = ExtractFirstTextPart(result.stdout_text);
    const auto summary = result.success ? (extracted_text.empty() ? result.stdout_text : extracted_text) : "";
    const auto legacy_output = MakeJsonObject({
        {"agent", QuoteJson("gemini")},
        {"provider", QuoteJson("gemini")},
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
        .summary = result.success ? summary : "Gemini generateContent request failed.",
        .structured_output_json = legacy_output,
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "gemini",
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

AgentResult GeminiAgent::run_task_with_cli_session(
    const AgentTask& task,
    const AuthSession& session,
    const std::filesystem::path& workspace_path) {
    const auto started_at = std::chrono::steady_clock::now();

    if (!CommandExists("gemini") && !CommandExists("gemini.cmd")) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = "gemini CLI was not found on PATH",
        };
    }

    const auto model = model_name(task);
    const CliSpec spec{
        .name = "gemini_cli_agent",
        .description = "Run Gemini CLI in non-interactive mode using its external OAuth session.",
        .binary = "gemini",
        .args_template = {
            "-p",
            "{{prompt}}",
            "--output-format",
            "text",
            "--skip-trust",
            "-m",
            "{{model}}",
        },
        .required_args = {"prompt", "model"},
        .input_schema_json = R"({"type":"object","required":["prompt"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"process.spawn", "network.access"},
        .timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : 120000,
        .output_limit_bytes = 1024 * 1024,
        .env_allowlist = {
            "USERPROFILE",
            "HOMEDRIVE",
            "HOMEPATH",
            "HOME",
            "APPDATA",
            "LOCALAPPDATA",
            "XDG_CONFIG_HOME",
            "GEMINI_CONFIG_DIR",
            "GOOGLE_APPLICATION_CREDENTIALS",
        },
    };

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"prompt", BuildPrompt(task)},
            {"model", model},
        },
        .workspace_path = workspace_path,
    });

    const auto legacy_output = MakeJsonObject({
        {"agent", QuoteJson("gemini")},
        {"provider", QuoteJson("gemini")},
        {"profile", QuoteJson(session.profile_name)},
        {"auth_source", QuoteJson("gemini_cli_oauth")},
        {"model", QuoteJson(model)},
        {"content", QuoteJson(result.success ? result.stdout_text : "")},
        {"command", QuoteJson(result.command_display)},
        {"exit_code", NumberAsJson(result.exit_code)},
        {"stdout", QuoteJson(result.stdout_text)},
        {"stderr", QuoteJson(result.stderr_text)},
    });
    AgentResult agent_result{
        .success = result.success,
        .summary = result.success ? result.stdout_text : "Gemini CLI task failed.",
        .structured_output_json = legacy_output,
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
    agent_result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "gemini",
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

AgentResult GeminiAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool GeminiAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

std::string GeminiAgent::profile_name() const {
    return profile_store_.default_profile(AuthProviderId::gemini).value_or("default");
}

std::string GeminiAgent::model_name(const AgentTask& task) {
    const auto marker = task.constraints_json.find("\"model\":\"");
    if (marker != std::string::npos) {
        const auto start = marker + 9;
        const auto end = task.constraints_json.find('"', start);
        if (end != std::string::npos && end > start) {
            return NormalizeGeminiModelName(task.constraints_json.substr(start, end - start));
        }
    }
    return kDefaultGeminiModel;
}

std::string GeminiAgent::BuildPrompt(const AgentTask& task) {
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

std::string GeminiAgent::BuildRequestBody(const AgentTask& task) {
    return MakeJsonObject({
        {"contents", "[{\"role\":\"user\",\"parts\":[{\"text\":" + QuoteJson(BuildPrompt(task)) + "}]}]"},
    });
}

std::string GeminiAgent::ExtractFirstTextPart(const std::string& response_json) {
    const auto text_key = response_json.find("\"text\"");
    if (text_key == std::string::npos) {
        return "";
    }
    const auto colon = response_json.find(':', text_key);
    if (colon == std::string::npos) {
        return "";
    }
    return JsonStringValueAt(response_json, colon + 1).value_or("");
}

}  // namespace agentos
