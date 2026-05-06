#include "hosts/agents/main_agent.hpp"

#include "core/orchestration/agent_result_normalizer.hpp"
#include "utils/command_utils.hpp"
#include "utils/curl_secret.hpp"
#include "utils/path_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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

// Returns the mid-section of a JWT (the base64url-encoded claims) decoded
// into a JSON value. Empty json on any parse/format failure.
nlohmann::json DecodeJwtClaims(const std::string& jwt) {
    const auto first_dot = jwt.find('.');
    if (first_dot == std::string::npos) return {};
    const auto second_dot = jwt.find('.', first_dot + 1);
    if (second_dot == std::string::npos) return {};
    std::string payload = jwt.substr(first_dot + 1, second_dot - first_dot - 1);
    // base64url -> base64
    for (auto& c : payload) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (payload.size() % 4) payload += '=';
    // Decode base64. nlohmann doesn't ship base64 — implement minimally.
    static const std::string kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int val = 0;
    int bits = -8;
    for (const char c : payload) {
        if (c == '=') break;
        const auto idx = kAlphabet.find(c);
        if (idx == std::string::npos) return {};
        val = (val << 6) | static_cast<int>(idx);
        bits += 6;
        if (bits >= 0) {
            decoded += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    try {
        return nlohmann::json::parse(decoded);
    } catch (...) {
        return {};
    }
}

struct OAuthFileContents {
    std::string access_token;
    std::string refresh_token;
    std::string client_id;        // extracted from id_token's `azp` claim
    long long expiry_date_ms = 0; // unix ms; 0 means unknown
    nlohmann::json raw;            // full parsed file, used to re-serialize on refresh
    std::string error;             // populated if read failed
};

OAuthFileContents ReadOAuthFile(const std::filesystem::path& path) {
    OAuthFileContents out;
    std::ifstream in(path);
    if (!in) {
        out.error = "oauth_file path is not readable: " + path.string();
        return out;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    try {
        out.raw = nlohmann::json::parse(buf.str());
    } catch (const std::exception& e) {
        out.error = std::string("oauth_file is not valid JSON: ") + e.what();
        return out;
    }
    if (out.raw.contains("access_token") && out.raw["access_token"].is_string()) {
        out.access_token = out.raw["access_token"].get<std::string>();
    }
    if (out.raw.contains("refresh_token") && out.raw["refresh_token"].is_string()) {
        out.refresh_token = out.raw["refresh_token"].get<std::string>();
    }
    if (out.access_token.empty() && out.raw.contains("tokens") && out.raw["tokens"].is_object()) {
        const auto& tokens = out.raw["tokens"];
        if (tokens.contains("access_token") && tokens["access_token"].is_string()) {
            out.access_token = tokens["access_token"].get<std::string>();
        }
        if (tokens.contains("refresh_token") && tokens["refresh_token"].is_string()) {
            out.refresh_token = tokens["refresh_token"].get<std::string>();
        }
        if (out.raw.contains("last_refresh") && out.raw["last_refresh"].is_string()) {
            out.raw["credential_source"] = "codex_cli_oauth";
        }
    }
    if (out.raw.contains("expiry_date") && out.raw["expiry_date"].is_number_integer()) {
        out.expiry_date_ms = out.raw["expiry_date"].get<long long>();
    }
    if (out.raw.contains("id_token") && out.raw["id_token"].is_string()) {
        const auto claims = DecodeJwtClaims(out.raw["id_token"].get<std::string>());
        if (claims.is_object() && claims.contains("azp") && claims["azp"].is_string()) {
            out.client_id = claims["azp"].get<std::string>();
        }
    }
    if (out.access_token.empty()) {
        out.error = "oauth_file is missing access_token field";
    }
    return out;
}

long long NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// We don't refresh OAuth tokens ourselves: the Google flow that the
// gemini CLI uses requires a client_secret bundled inside the CLI's
// own binary, so a refresh request from outside the CLI gets back
// "invalid_request: client_secret is missing." Instead, detect
// expiry and surface a clear "re-run gemini auth login" instruction
// — letting the CLI that owns the credentials refresh them.
//
// Returns true when the saved token is fresh enough to use, false
// when expiry has passed (with `contents.error` populated).
bool ValidateOAuthFreshness(OAuthFileContents& contents) {
    if (contents.expiry_date_ms == 0) {
        // Unknown expiry — assume the file owner manages freshness.
        return true;
    }
    constexpr long long kRefreshSkewMs = 60 * 1000;  // 1 min slack
    const auto now_ms = NowEpochMs();
    if (contents.expiry_date_ms - now_ms > kRefreshSkewMs) {
        return true;
    }
    contents.error =
        "oauth_file access_token has expired (expiry was " +
        std::to_string(contents.expiry_date_ms) +
        " ms epoch). Re-run the CLI that owns this credentials file "
        "(e.g. `gemini auth login` for ~/.gemini/oauth_creds.json) "
        "to mint a fresh token, then try again.";
    return false;
}

// Render a compact one-line-per-entry catalog of skills/agents so the
// chat preamble can ground the model in what AgentOS can actually do.
// Capped at kMaxListed entries with a "… and N more" tail so a runtime
// with hundreds of plugin skills can't blow up the prompt.
constexpr std::size_t kMaxListed = 40;

std::string FirstSentence(const std::string& text) {
    if (text.empty()) return {};
    const auto dot = text.find('.');
    if (dot == std::string::npos) return text;
    auto head = text.substr(0, dot);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) {
        head.pop_back();
    }
    if (head.empty()) return text;
    return head;
}

std::string DerivedUseHint(const SkillManifest& m) {
    for (const auto& cap : m.capabilities) {
        if (cap == "filesystem") return "you need to read/write a workspace file";
        if (cap == "network") return "you need to fetch external content";
        if (cap == "introspection" || cap == "host") return "the user is asking about this machine";
    }
    return FirstSentence(m.description);
}

std::string ParseRequiredInputs(const std::string& input_schema_json) {
    if (input_schema_json.empty()) return "(none)";
    try {
        const auto j = nlohmann::json::parse(input_schema_json);
        if (!j.is_object() || !j.contains("required")) return "(none)";
        const auto& req = j["required"];
        if (!req.is_array() || req.empty()) return "(none)";
        std::string out;
        for (const auto& entry : req) {
            if (!entry.is_string()) continue;
            if (!out.empty()) out += ", ";
            out += entry.get<std::string>();
        }
        return out.empty() ? std::string("(none)") : out;
    } catch (...) {
        return "(unknown)";
    }
}

std::string FormatRegisteredSkills(const SkillRegistry* registry) {
    if (registry == nullptr) return "  (skill registry unavailable)\n";
    const auto skills = registry->list();
    if (skills.empty()) return "  (none registered)\n";
    std::ostringstream out;
    const auto shown = std::min(skills.size(), kMaxListed);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& m = skills[i];
        const auto risk = m.risk_level.empty() ? std::string("unknown") : m.risk_level;
        const auto desc = FirstSentence(m.description);
        out << "  " << m.name << " [risk=" << risk << "]";
        if (!desc.empty()) out << " — " << desc;
        out << "\n";
        out << "      use when: " << DerivedUseHint(m) << "\n";
        out << "      required: " << ParseRequiredInputs(m.input_schema_json) << "\n";
    }
    if (skills.size() > kMaxListed) {
        out << "  ... and " << (skills.size() - kMaxListed) << " more\n";
    }
    return out.str();
}

std::string FormatRegisteredAgents(const AgentRegistry* registry) {
    if (registry == nullptr) return "  (agent registry unavailable)\n";
    const auto profiles = registry->list_profiles();
    if (profiles.empty()) return "  (none registered)\n";
    std::ostringstream out;
    const auto shown = std::min(profiles.size(), kMaxListed);
    for (std::size_t i = 0; i < shown; ++i) {
        const auto& p = profiles[i];
        out << "  " << p.agent_name
            << " [cost=" << (p.cost_tier.empty() ? std::string("unknown") : p.cost_tier)
            << ", streaming=" << (p.supports_streaming ? "true" : "false") << "]\n";
    }
    if (profiles.size() > kMaxListed) {
        out << "  ... and " << (profiles.size() - kMaxListed) << " more\n";
    }
    return out.str();
}

std::string BuildPromptTextImpl(const AgentTask& task,
                                const SkillRegistry* skill_registry,
                                const AgentRegistry* agent_registry) {
    if (task.task_type == "chat") {
        const auto skills_text = FormatRegisteredSkills(skill_registry);
        const auto agents_text = FormatRegisteredAgents(agent_registry);
        const auto skill_count = skill_registry ? skill_registry->list().size() : 0;
        const auto agent_count = agent_registry ? agent_registry->list_profiles().size() : 0;
        std::ostringstream out;
        out << "You are the AgentOS local runtime assistant. You are running inside a C++ "
               "AgentOS process on the user's own machine — you are NOT a cloud service, "
               "you do not have your own IP, and you should not claim to be ChatGPT, "
               "Qwen-as-cloud-product, or any vendor's hosted chatbot. The user is "
               "interacting with you via an interactive REPL that dispatches free-form "
               "text to you.\n\n"
               "When users ask about your capabilities or skills, refer to the registered "
               "AgentOS skills and agents listed below — these are what you can actually "
               "invoke through this runtime. Do not invent skills you don't see in the "
               "list.\n\n"
               "When users ask about the host machine (IP, hostname, files, network), "
               "explain that you can answer such questions by invoking the appropriate "
               "registered skill (e.g. `host_info` if available), not by guessing.\n\n"
            << "Registered skills (" << skill_count << "):\n"
            << skills_text
            << "\nRegistered agents (" << agent_count << "):\n"
            << agents_text
            << "\nUser: " << task.objective;
        return out.str();
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

std::string TrimWhitespace(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string StripLeadingThinkBlock(std::string value) {
    value = TrimWhitespace(std::move(value));
    constexpr const char* kOpen = "<think>";
    constexpr const char* kClose = "</think>";
    if (value.rfind(kOpen, 0) != 0) {
        return value;
    }
    const auto close = value.find(kClose);
    if (close == std::string::npos) {
        return value;
    }
    return TrimWhitespace(value.substr(close + std::string(kClose).size()));
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
    if (provider_kind == "openai-chat") return StripLeadingThinkBlock(ExtractOpenAiContent(response_json));
    if (provider_kind == "anthropic-messages") return StripLeadingThinkBlock(ExtractAnthropicContent(response_json));
    if (provider_kind == "gemini-generatecontent" || provider_kind == "vertex-gemini") {
        return StripLeadingThinkBlock(ExtractGeminiContent(response_json));
    }
    return {};
}

}  // namespace

std::string BuildMainAgentPrompt(const AgentTask& task,
                                 const SkillRegistry* skill_registry,
                                 const AgentRegistry* agent_registry) {
    return BuildPromptTextImpl(task, skill_registry, agent_registry);
}

MainAgent::MainAgent(const CliHost& cli_host,
                     MainAgentStore store,
                     std::filesystem::path workspace_root,
                     const SkillRegistry& skill_registry,
                     const AgentRegistry& agent_registry)
    : cli_host_(cli_host),
      store_(std::move(store)),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))),
      skill_registry_(&skill_registry),
      agent_registry_(&agent_registry) {}

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
    // Quick token presence check WITHOUT going through ResolveToken,
    // because ResolveToken triggers a network refresh on near-expiry
    // OAuth files — we don't want `agentos agents` to spend 30s on
    // a Google round-trip just to print a health column. The actual
    // refresh happens lazily at first run_task() call.
    if (!config->api_key_env.empty()) {
        return !ReadEnv(config->api_key_env).empty();
    }
    if (!config->api_key.empty()) {
        return true;
    }
    if (!config->oauth_file.empty()) {
        const auto contents = ReadOAuthFile(config->oauth_file);
        return contents.error.empty() && !contents.access_token.empty();
    }
    return false;
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
    if (!config.api_key.empty()) {
        // Literal api_key — primarily for local/self-hosted endpoints
        // (llama.cpp / vLLM / LM Studio etc.) that ignore the value
        // but still expect a non-empty Bearer header. Real provider
        // secrets should still go through api_key_env so the value
        // doesn't sit on disk in the workspace.
        return {config.api_key, {}, {}};
    }
    if (!config.oauth_file.empty()) {
        auto contents = ReadOAuthFile(config.oauth_file);
        if (!contents.error.empty()) {
            return {{}, "AuthUnavailable", contents.error};
        }
        if (!ValidateOAuthFreshness(contents)) {
            return {{}, "AuthExpired", contents.error};
        }
        return {contents.access_token, {}, {}};
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

    const auto prompt = BuildMainAgentPrompt(task, skill_registry_, agent_registry_);
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
