#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace agentos {

// Configuration for the primary chat agent.
//
// Unlike the five existing sub-agents (which serve as orchestrated
// secondary specialists), the main agent is a single REST endpoint that
// the REPL chat path dispatches through. It owns its own auth — either
// an env-ref API key (cheap, common case) or a path to an OAuth
// credentials file (e.g. ~/.gemini/oauth_creds.json) whose `access_token`
// field is read at request time.
//
// `provider_kind` selects the request shape since each provider has a
// different API surface:
//   - "openai-chat"           POST {base_url}/chat/completions, Bearer auth
//                             (works for OpenAI, DeepSeek, Together, Qwen
//                             Model Studio's openai-compatible endpoint)
//   - "anthropic-messages"    POST {base_url}/v1/messages, x-api-key header
//   - "gemini-generatecontent" POST {base_url}/models/{model}:generateContent
struct MainAgentConfig {
    std::string provider_kind;     // "openai-chat" | "anthropic-messages" | "gemini-generatecontent"
    std::string base_url;          // e.g. "https://api.openai.com/v1"
    std::string api_key_env;       // env var name; mutually exclusive-ish with oauth_file
    std::string oauth_file;        // path to JSON file with `access_token` field
    std::string model;             // e.g. "gpt-4o" / "claude-sonnet-4-5" / "gemini-2.5-pro"
    int default_timeout_ms = 120000;

    bool empty() const {
        return provider_kind.empty() && base_url.empty()
            && api_key_env.empty() && oauth_file.empty() && model.empty();
    }
};

class MainAgentStore {
public:
    explicit MainAgentStore(std::filesystem::path path);

    // Returns nullopt when no record exists. Returns the parsed record
    // otherwise. Malformed records also return nullopt — callers should
    // re-run `main-agent set ...` to fix them.
    std::optional<MainAgentConfig> load() const;
    bool save(const MainAgentConfig& config) const;
    bool clear() const;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace agentos
