#include "hosts/agents/main_agent.hpp"

#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/command_utils.hpp"
#include "utils/curl_secret.hpp"
#include "utils/path_utils.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace agentos {

namespace {

long ElapsedMs(const std::chrono::steady_clock::time_point& started_at) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started_at)
                                 .count());
}

std::string ReadEnv(const std::string& name) {
    if (name.empty()) return {};
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name.c_str()) == 0 && buf) {
        std::string out(buf);
        free(buf);
        return out;
    }
    return {};
#else
    if (const char* env = std::getenv(name.c_str()); env != nullptr) {
        return env;
    }
    return {};
#endif
}

// Reads `access_token` from a JSON file, e.g. ~/.gemini/oauth_creds.json.
// Returns empty string when file/field is missing or malformed.
std::string ReadOAuthFileAccessToken(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream buf;
    buf << in.rdbuf();
    try {
        const auto j = nlohmann::json::parse(buf.str());
        if (j.contains("access_token") && j["access_token"].is_string()) {
            return j["access_token"].get<std::string>();
        }
    } catch (...) {
    }
    return {};
}

// "Hello" -> "User: Hello" for chat-style task_type. Other task_types get
// the agentos orchestration scaffolding so the main agent can also serve
// as a generic provider when invoked through `agentos run target=main`.
std::string BuildPromptText(const AgentTask& task) {
    if (task.task_type == "chat") {
        return "You are a helpful chat assistant. Reply naturally and concisely.\n\nUser: " +
               task.objective;
    }
    std::ostringstream out;
    out << "Task type: " << task.task_type << "\n"
        << "Objective: " << task.objective << "\n";
    if (!task.context_json.empty()) out << "Context JSON: " << task.context_json << "\n";
    if (!task.constraints_json.empty()) out << "Constraints JSON: " << task.constraints_json << "\n";
    return out.str();
}

struct PreparedRequest {
    std::string url;
    std::string body_json;
    std::vector<std::string> header_lines;  // each "Name: value"
};

PreparedRequest PrepareOpenAiChat(const MainAgentConfig& config,
                                  const std::string& token,
                                  const std::string& prompt) {
    nlohmann::ordered_json body;
    body["model"] = config.model;
    body["messages"] = nlohmann::json::array({
        nlohmann::ordered_json{{"role", "user"}, {"content", prompt}},
    });
    PreparedRequest req;
    req.url = config.base_url;
    if (!req.url.empty() && req.url.back() == '/') req.url.pop_back();
    req.url += "/chat/completions";
    req.body_json = body.dump();
    req.header_lines = {"Authorization: Bearer " + token};
    return req;
}

PreparedRequest PrepareAnthropicMessages(const MainAgentConfig& config,
                                         const std::string& token,
                                         const std::string& prompt) {
    nlohmann::ordered_json body;
    body["model"] = config.model;
    body["max_tokens"] = 4096;
    body["messages"] = nlohmann::json::array({
        nlohmann::ordered_json{{"role", "user"}, {"content", prompt}},
    });
    PreparedRequest req;
    req.url = config.base_url;
    if (!req.url.empty() && req.url.back() == '/') req.url.pop_back();
    req.url += "/v1/messages";
    req.body_json = body.dump();
    req.header_lines = {
        "x-api-key: " + token,
        "anthropic-version: 2023-06-01",
    };
    return req;
}

PreparedRequest PrepareGeminiGenerate(const MainAgentConfig& config,
                                      const std::string& token,
                                      const std::string& prompt) {
    nlohmann::ordered_json part;
    part["text"] = prompt;
    nlohmann::ordered_json content;
    content["role"] = "user";
    content["parts"] = nlohmann::ordered_json::array({part});
    nlohmann::ordered_json body;
    body["contents"] = nlohmann::ordered_json::array({content});

    PreparedRequest req;
    req.url = config.base_url;
    if (!req.url.empty() && req.url.back() == '/') req.url.pop_back();
    req.url += "/models/" + config.model + ":generateContent";
    req.body_json = body.dump();
    req.header_lines = {"Authorization: Bearer " + token};
    return req;
}

// Vertex AI shape — same body schema as gemini-generatecontent but
// the URL is parameterized by GCP project + region, and it accepts
// the cloud-platform-scoped OAuth tokens that the gemini CLI's
// oauth_creds.json file contains.
PreparedRequest PrepareVertexGemini(const MainAgentConfig& config,
                                    const std::string& token,
                                    const std::string& prompt) {
    nlohmann::ordered_json part;
    part["text"] = prompt;
    nlohmann::ordered_json content;
    content["role"] = "user";
    content["parts"] = nlohmann::ordered_json::array({part});
    nlohmann::ordered_json body;
    body["contents"] = nlohmann::ordered_json::array({content});

    PreparedRequest req;
    // base_url defaults to https://{location}-aiplatform.googleapis.com when
    // empty so users only need to set project + location for the typical case.
    std::string base = config.base_url;
    if (base.empty()) {
        base = "https://" + config.location + "-aiplatform.googleapis.com";
    }
    if (!base.empty() && base.back() == '/') base.pop_back();
    req.url = base + "/v1/projects/" + config.project_id +
              "/locations/" + config.location +
              "/publishers/google/models/" + config.model + ":generateContent";
    req.body_json = body.dump();
    req.header_lines = {"Authorization: Bearer " + token};
    return req;
}

std::string ExtractOpenAiContent(const std::string& response_json) {
    try {
        const auto j = nlohmann::json::parse(response_json);
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
            const auto& first = j["choices"][0];
            if (first.contains("message") && first["message"].contains("content") &&
                first["message"]["content"].is_string()) {
                return first["message"]["content"].get<std::string>();
            }
        }
    } catch (...) {}
    return {};
}

std::string ExtractAnthropicContent(const std::string& response_json) {
    try {
        const auto j = nlohmann::json::parse(response_json);
        if (j.contains("content") && j["content"].is_array()) {
            for (const auto& part : j["content"]) {
                if (part.contains("type") && part["type"] == "text" &&
                    part.contains("text") && part["text"].is_string()) {
                    return part["text"].get<std::string>();
                }
            }
        }
    } catch (...) {}
    return {};
}

std::string ExtractGeminiContent(const std::string& response_json) {
    try {
        const auto j = nlohmann::json::parse(response_json);
        if (j.contains("candidates") && j["candidates"].is_array() && !j["candidates"].empty()) {
            const auto& first = j["candidates"][0];
            if (first.contains("content") && first["content"].contains("parts") &&
                first["content"]["parts"].is_array()) {
                for (const auto& part : first["content"]["parts"]) {
                    if (part.contains("text") && part["text"].is_string()) {
                        return part["text"].get<std::string>();
                    }
                }
            }
        }
    } catch (...) {}
    return {};
}

std::string ExtractContent(const std::string& provider_kind, const std::string& response_json) {
    if (provider_kind == "openai-chat") return ExtractOpenAiContent(response_json);
    if (provider_kind == "anthropic-messages") return ExtractAnthropicContent(response_json);
    if (provider_kind == "gemini-generatecontent" || provider_kind == "vertex-gemini") {
        return ExtractGeminiContent(response_json);
    }
    return {};
}

}  // namespace

MainAgent::MainAgent(const CliHost& cli_host,
                     MainAgentStore store,
                     std::filesystem::path workspace_root)
    : cli_host_(cli_host),
      store_(std::move(store)),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))) {}

AgentProfile MainAgent::profile() const {
    return {
        .agent_name = "main",
        .version = "0.1.0",
        .description = "Primary chat agent — REST adapter configured via `agentos main-agent set`.",
        .capabilities = {{"chat", 95}, {"analysis", 80}},
        .supports_session = false,
        .supports_streaming = false,
        .supports_patch = false,
        .supports_subagents = false,
        .supports_network = true,
        .cost_tier = "variable",
        .latency_tier = "medium",
        .risk_level = "medium",
    };
}

bool MainAgent::healthy() const {
    if (!CommandExists("curl") && !CommandExists("curl.exe")) return false;
    const auto config = store_.load();
    if (!config.has_value()) return false;
    if (config->model.empty() || config->provider_kind.empty()) {
        return false;
    }
    // base_url is optional for vertex-gemini (defaults to
    // {location}-aiplatform.googleapis.com), required otherwise.
    if (config->base_url.empty() && config->provider_kind != "vertex-gemini") {
        return false;
    }
    if (config->provider_kind == "vertex-gemini" &&
        (config->project_id.empty() || config->location.empty())) {
        return false;
    }
    const auto token = ResolveToken(*config);
    return token.error_code.empty() && !token.token.empty();
}

MainAgent::TokenResolution MainAgent::ResolveToken(const MainAgentConfig& config) const {
    if (!config.api_key_env.empty()) {
        const auto value = ReadEnv(config.api_key_env);
        if (value.empty()) {
            return {{}, "AuthUnavailable",
                    "main-agent api_key_env=" + config.api_key_env +
                        " is not set in the current environment"};
        }
        return {value, {}, {}};
    }
    if (!config.oauth_file.empty()) {
        const auto value = ReadOAuthFileAccessToken(config.oauth_file);
        if (value.empty()) {
            return {{}, "AuthUnavailable",
                    "main-agent oauth_file=" + config.oauth_file +
                        " is missing or has no access_token field"};
        }
        return {value, {}, {}};
    }
    return {{}, "AuthUnavailable",
            "main-agent has neither api_key_env nor oauth_file configured"};
}

AgentResult MainAgent::run_task(const AgentTask& task) {
    const auto started_at = std::chrono::steady_clock::now();

    const auto config_opt = store_.load();
    if (!config_opt.has_value()) {
        return {.success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "NotConfigured",
                .error_message = "main-agent is not configured; run `agentos main-agent set ...` first"};
    }
    const auto& config = *config_opt;

    if (config.provider_kind != "openai-chat" &&
        config.provider_kind != "anthropic-messages" &&
        config.provider_kind != "gemini-generatecontent" &&
        config.provider_kind != "vertex-gemini") {
        return {.success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "ConfigInvalid",
                .error_message = "main-agent provider_kind must be one of "
                                 "openai-chat / anthropic-messages / "
                                 "gemini-generatecontent / vertex-gemini"};
    }
    if (config.provider_kind == "vertex-gemini" &&
        (config.project_id.empty() || config.location.empty())) {
        return {.success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "ConfigInvalid",
                .error_message = "vertex-gemini requires project_id= and location="};
    }

    if (!CommandExists("curl") && !CommandExists("curl.exe")) {
        return {.success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "AgentUnavailable",
                .error_message = "curl was not found on PATH"};
    }

    const auto token_resolution = ResolveToken(config);
    if (!token_resolution.error_code.empty()) {
        return {.success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = token_resolution.error_code,
                .error_message = token_resolution.error_message};
    }

    const auto prompt = BuildPromptText(task);
    PreparedRequest prepared;
    if (config.provider_kind == "openai-chat") {
        prepared = PrepareOpenAiChat(config, token_resolution.token, prompt);
    } else if (config.provider_kind == "anthropic-messages") {
        prepared = PrepareAnthropicMessages(config, token_resolution.token, prompt);
    } else if (config.provider_kind == "vertex-gemini") {
        prepared = PrepareVertexGemini(config, token_resolution.token, prompt);
    } else {
        prepared = PrepareGeminiGenerate(config, token_resolution.token, prompt);
    }

    // Write body and headers to a tempdir under workspace_root_ so the bearer
    // token never reaches argv. Cleaned up by CurlSecretFiles destructor.
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(workspace_root_, prepared.body_json, prepared.header_lines);
    } catch (const std::exception& e) {
        return {.success = false,
                .duration_ms = ElapsedMs(started_at),
                .error_code = "InternalError",
                .error_message = std::string("failed to stage curl request files: ") + e.what()};
    }

    const int timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : config.default_timeout_ms;

    const CliSpec spec{
        .name = "main_agent_request",
        .description = "Call the configured main-agent REST endpoint.",
        .binary = "curl",
        .args_template = {
            "-L", "--silent", "--show-error", "--fail-with-body",
            "--max-time", "{{max_time_seconds}}",
            "{{url}}",
            "-H", "Content-Type: application/json",
            "-H", "{{auth_headers_file}}",
            "-X", "POST",
            "-d", "{{request_body_file}}",
        },
        .required_args = {"url", "auth_headers_file", "request_body_file", "max_time_seconds"},
        .input_schema_json = R"({"type":"object","required":["url","request_body_file"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = timeout_ms,
        .output_limit_bytes = 4 * 1024 * 1024,
    };

    const auto cli_result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"url", prepared.url},
            // Keep the actual token in the argument map so SecretRedactor
            // can scrub it from the recorded command_display even though
            // it never reaches argv (we pass --data @file / -H @file).
            {"api_key", token_resolution.token},
            {"auth_headers_file", std::string("@") + secret_files.headers_file.string()},
            {"request_body_file", std::string("@") + secret_files.body_file.string()},
            {"max_time_seconds", std::to_string(std::max(1, timeout_ms / 1000))},
        },
        .workspace_path = workspace_root_,
    });

    const auto extracted = ExtractContent(config.provider_kind, cli_result.stdout_text);
    const auto summary = cli_result.success
                             ? (extracted.empty() ? cli_result.stdout_text : extracted)
                             : "";

    nlohmann::ordered_json legacy;
    legacy["agent"] = "main";
    legacy["provider_kind"] = config.provider_kind;
    legacy["base_url"] = config.base_url;
    legacy["model"] = config.model;
    legacy["content"] = summary;
    legacy["command"] = cli_result.command_display;
    legacy["exit_code"] = cli_result.exit_code;
    legacy["response"] = cli_result.stdout_text;
    legacy["stderr"] = cli_result.stderr_text;

    // On failure, surface the provider's actual error text in
    // error_message rather than the generic "CLI command exited with
    // a non-zero status" — providers return JSON like {"error":{
    // "code":403,"message":"..."}} on the body even with curl
    // --fail-with-body, so the response body is the most useful
    // diagnostic. Truncate aggressively so a noisy provider can't
    // flood the audit log.
    std::string failure_message = cli_result.error_message;
    if (!cli_result.success) {
        const std::string& body = cli_result.stdout_text.empty()
                                      ? cli_result.stderr_text
                                      : cli_result.stdout_text;
        if (!body.empty()) {
            constexpr size_t kMaxBody = 600;
            failure_message = "main-agent request failed: ";
            failure_message += body.substr(0, kMaxBody);
            if (body.size() > kMaxBody) failure_message += "... [truncated]";
        }
    }

    AgentResult result{
        .success = cli_result.success,
        .summary = cli_result.success ? summary : "main-agent request failed.",
        .structured_output_json = legacy.dump(),
        .duration_ms = cli_result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = cli_result.error_code,
        .error_message = failure_message,
    };
    result.structured_output_json = BuildNormalizedAgentResultJson(NormalizedAgentResultInput{
        .agent_name = "main",
        .success = result.success,
        .summary = result.summary,
        .structured_output_json = legacy.dump(),
        .artifacts = result.artifacts,
        .duration_ms = result.duration_ms,
        .estimated_cost = result.estimated_cost,
        .error_code = result.error_code,
        .error_message = result.error_message,
    });
    return result;
}

std::string MainAgent::start_session(const std::string& /*session_config_json*/) {
    // Stateless REST adapter — no real session, but return a stable
    // synthetic id so callers that demand a session don't see an empty
    // string and assume failure.
    return "main-stateless";
}

void MainAgent::close_session(const std::string& /*session_id*/) {}

AgentResult MainAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool MainAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

}  // namespace agentos
