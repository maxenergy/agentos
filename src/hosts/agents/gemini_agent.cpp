#include "hosts/agents/gemini_agent.hpp"

#include "auth/auth_models.hpp"
#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/cancellation.hpp"
#include "utils/command_utils.hpp"
#include "utils/curl_secret.hpp"
#include "utils/path_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
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
    const auto session = credential_broker_.get_session(AuthProviderId::gemini, profile_name(std::nullopt));
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

    const auto requested_workspace = task.workspace_path.empty()
        ? workspace_root_
        : std::filesystem::path(task.workspace_path);
    const auto workspace_path = NormalizeWorkspaceRoot(requested_workspace);
    std::error_code workspace_ec;
    if (!std::filesystem::exists(workspace_path, workspace_ec) ||
        !std::filesystem::is_directory(workspace_path, workspace_ec)) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "InvalidWorkspace",
            .error_message = "agent workspace must be an existing directory",
        };
    }

    const auto profile = profile_name(task.auth_profile);
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

    const auto profile = profile_name(task.auth_profile);
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

    // Stage the request body and the auth header to short-lived files under
    // runtime/.tmp/. Passing `-d @file` and `-H @file` keeps the bearer token
    // / API key (and the request body) out of the curl process command line,
    // where they would otherwise be visible to other local users via
    // /proc/<pid>/cmdline or Windows process listings.
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(workspace_path, body, /*header_lines=*/{auth_header});
    } catch (const std::exception& error) {
        return {
            .success = false,
            .duration_ms = ElapsedMs(started_at),
            .error_code = "AgentUnavailable",
            .error_message = std::string("could not stage gemini request: ") + error.what(),
        };
    }
    const auto body_arg = std::string("@") + secret_files.body_file.string();
    const auto headers_arg = std::string("@") + secret_files.headers_file.string();

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
            "{{auth_headers_file}}",
            "-X",
            "POST",
            "-d",
            "{{request_body_file}}",
        },
        .required_args = {"url", "auth_headers_file", "request_body_file", "max_time_seconds"},
        .input_schema_json = R"({"type":"object","required":["url","request_body_file"]})",
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
            {"auth_headers_file", headers_arg},
            // Keep `api_key` and `auth_header` in the argument map even though
            // they are no longer rendered into argv: SecretRedactor mines the
            // map for sensitive field names so it can scrub the token from
            // command_display, stdout, and stderr.
            {"api_key", token},
            {"auth_header", auth_header},
            {"request_body_file", body_arg},
            {"max_time_seconds", std::to_string(std::max(1, spec.timeout_ms / 1000))},
        },
        .workspace_path = workspace_path,
    });

    const auto extracted_text = ExtractFirstTextPart(result.stdout_text);
    const auto summary = result.success ? (extracted_text.empty() ? result.stdout_text : extracted_text) : "";
    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "gemini";
    legacy_output_json["provider"] = "gemini";
    legacy_output_json["profile"] = profile;
    legacy_output_json["model"] = model;
    legacy_output_json["content"] = summary;
    legacy_output_json["command"] = result.command_display;
    legacy_output_json["exit_code"] = result.exit_code;
    legacy_output_json["response"] = result.stdout_text;
    legacy_output_json["stderr"] = result.stderr_text;
    const auto legacy_output = legacy_output_json.dump();
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

    nlohmann::ordered_json legacy_output_json;
    legacy_output_json["agent"] = "gemini";
    legacy_output_json["provider"] = "gemini";
    legacy_output_json["profile"] = session.profile_name;
    legacy_output_json["auth_source"] = "gemini_cli_oauth";
    legacy_output_json["model"] = model;
    legacy_output_json["content"] = result.success ? result.stdout_text : "";
    legacy_output_json["command"] = result.command_display;
    legacy_output_json["exit_code"] = result.exit_code;
    legacy_output_json["stdout"] = result.stdout_text;
    legacy_output_json["stderr"] = result.stderr_text;
    const auto legacy_output = legacy_output_json.dump();
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

bool GeminiAgent::cancel(const std::string& /*task_id*/) {
    // Legacy cancel(task_id) is a no-op stub. V2 callers drive cancellation
    // via AgentInvocation::cancel (CancellationToken).
    return false;
}

// ---------------------------------------------------------------------------
// V2 invoke path: wraps the existing run_task pipeline with a CancellationToken
// pre-check and SessionInit/Status/Final events. Gemini's REST + CLI paths are
// non-streaming today, so we do not attempt mid-call interruption — but the
// orchestrator gets a consistent V2 entry point and zero-cost Usage so the
// admission-control budget gate sees explicit data rather than missing fields.
// ---------------------------------------------------------------------------
AgentResult GeminiAgent::invoke(const AgentInvocation& invocation,
                                const AgentEventCallback& on_event) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count());
    };

    auto emit = [&](AgentEvent ev) {
        if (on_event) {
            (void)on_event(ev);
        }
    };

    if (invocation.cancel && invocation.cancel->is_cancelled()) {
        return {
            .success = false,
            .duration_ms = elapsed_ms(),
            .error_code = "Cancelled",
            .error_message = "gemini invocation was cancelled before dispatch",
        };
    }

    emit(AgentEvent{
        .kind = AgentEvent::Kind::SessionInit,
        .fields = {
            {"agent", "gemini"},
            {"profile", profile_name(invocation.auth_profile)},
            {"model", ModelNameFromConstraints(invocation.constraints)},
        },
    });
    emit(AgentEvent{
        .kind = AgentEvent::Kind::Status,
        .fields = {{"phase", "dispatch"}},
        .payload_text = "calling gemini provider",
    });

    auto result = run_task(TaskFromInvocation(invocation));

    // Gemini doesn't surface token usage in the current REST/CLI plumbing; emit
    // an explicit zero so the orchestrator's budget gate sees data rather than
    // missing fields. Cost/turns also default to zero.
    result.usage.cost_usd = 0.0;
    result.usage.turns = 1;
    if (invocation.session_id.has_value()) {
        result.session_id = invocation.session_id;
    }

    emit(AgentEvent{
        .kind = AgentEvent::Kind::Usage,
        .fields = {
            {"input_tokens", "0"},
            {"output_tokens", "0"},
            {"cost_usd", "0.0"},
            {"turns", "1"},
        },
    });
    emit(AgentEvent{
        .kind = AgentEvent::Kind::Final,
        .fields = {{"success", result.success ? "true" : "false"}},
        .payload_text = result.summary,
    });
    return result;
}

AgentTask GeminiAgent::TaskFromInvocation(const AgentInvocation& invocation) {
    nlohmann::json context_json = nlohmann::json::object();
    for (const auto& [k, v] : invocation.context) {
        context_json[k] = v;
    }
    nlohmann::json constraints_json = nlohmann::json::object();
    for (const auto& [k, v] : invocation.constraints) {
        constraints_json[k] = v;
    }
    AgentTask task;
    task.task_id = invocation.task_id;
    // task_type defaults to a stable string; legacy run_task path uses it for
    // prompt scaffolding only.
    const auto task_type_it = invocation.context.find("task_type");
    task.task_type = task_type_it == invocation.context.end()
        ? std::string{"agent_invoke"}
        : task_type_it->second;
    task.objective = invocation.objective;
    task.workspace_path = invocation.workspace_path.string();
    task.auth_profile = invocation.auth_profile;
    task.context_json = context_json.empty() ? std::string{} : context_json.dump();
    task.constraints_json = constraints_json.empty() ? std::string{} : constraints_json.dump();
    task.timeout_ms = invocation.timeout_ms;
    task.budget_limit = invocation.budget_limit_usd;
    return task;
}

std::string GeminiAgent::ModelNameFromConstraints(const StringMap& constraints) {
    const auto it = constraints.find("model");
    if (it == constraints.end() || it->second.empty()) {
        return kDefaultGeminiModel;
    }
    return NormalizeGeminiModelName(it->second);
}

std::string GeminiAgent::profile_name(const std::optional<std::string>& requested_profile) const {
    return requested_profile.value_or(profile_store_.default_profile(AuthProviderId::gemini).value_or("default"));
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

    // Chat mode skips the agent-orchestration preamble so the user's
    // message reaches the model verbatim. Without this, "hello" gets
    // wrapped in 6 lines of "You are running as a model provider agent
    // inside AgentOS / Task id / Workspace / Context JSON ..." and the
    // model answers in a stilted "I am ready to assist" register.
    if (task.task_type == "chat") {
        return "You are a helpful chat assistant. Reply naturally and concisely.\n\nUser: " + task.objective;
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
    nlohmann::ordered_json part;
    part["text"] = BuildPrompt(task);

    nlohmann::ordered_json content;
    content["role"] = "user";
    content["parts"] = nlohmann::ordered_json::array({part});

    nlohmann::ordered_json body;
    body["contents"] = nlohmann::ordered_json::array({content});
    return body.dump();
}

std::string GeminiAgent::ExtractFirstTextPart(const std::string& response_json) {
    try {
        const auto parsed = nlohmann::json::parse(response_json);
        const auto candidates = parsed.find("candidates");
        if (candidates == parsed.end() || !candidates->is_array()) {
            return "";
        }
        for (const auto& candidate : *candidates) {
            const auto content = candidate.find("content");
            if (content == candidate.end() || !content->is_object()) {
                continue;
            }
            const auto parts = content->find("parts");
            if (parts == content->end() || !parts->is_array()) {
                continue;
            }
            for (const auto& part : *parts) {
                const auto text = part.find("text");
                if (text != part.end() && text->is_string()) {
                    return text->get<std::string>();
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        return "";
    }
    return "";
}

}  // namespace agentos
