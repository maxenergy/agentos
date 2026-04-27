#include "cli/main_agent_commands.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::map<std::string, std::string> ParseKvArgs(int argc, char* argv[], int start) {
    std::map<std::string, std::string> options;
    for (int i = start; i < argc; ++i) {
        const std::string token = argv[i];
        const auto sep = token.find('=');
        if (sep == std::string::npos) {
            continue;
        }
        options[token.substr(0, sep)] = token.substr(sep + 1);
    }
    return options;
}

void PrintUsage() {
    std::cerr
        << "main-agent commands:\n"
        << "  agentos main-agent show\n"
        << "  agentos main-agent set provider=<openai-chat|anthropic-messages|gemini-generatecontent|vertex-gemini>\n"
        << "                         base_url=<url>          # vertex-gemini: optional, defaults to {location}-aiplatform.googleapis.com\n"
        << "                         model=<model_id>\n"
        << "                         [api_key_env=ENV_VAR]\n"
        << "                         [oauth_file=<path-to-json>]   # vertex-gemini accepts gemini CLI's oauth_creds.json\n"
        << "                         [project_id=<gcp-project>]    # vertex-gemini only\n"
        << "                         [location=<gcp-region>]       # vertex-gemini only, e.g. us-central1\n"
        << "                         [default_timeout_ms=120000]\n"
        << "  agentos main-agent unset\n"
        << "  agentos main-agent list-providers\n"
        << "\n"
        << "Examples:\n"
        << "  agentos main-agent set provider=openai-chat \\\n"
        << "    base_url=https://api.openai.com/v1 model=gpt-4o api_key_env=OPENAI_API_KEY\n"
        << "  agentos main-agent set provider=anthropic-messages \\\n"
        << "    base_url=https://api.anthropic.com model=claude-sonnet-4-5 \\\n"
        << "    api_key_env=ANTHROPIC_API_KEY\n"
        << "  agentos main-agent set provider=gemini-generatecontent \\\n"
        << "    base_url=https://generativelanguage.googleapis.com/v1beta \\\n"
        << "    model=gemini-2.5-flash api_key_env=GEMINI_API_KEY  # API key from aistudio.google.com/apikey\n"
        << "  agentos main-agent set provider=vertex-gemini \\\n"
        << "    project_id=my-gcp-project location=us-central1 \\\n"
        << "    model=gemini-2.5-flash oauth_file=C:/Users/you/.gemini/oauth_creds.json\n";
}

void PrintConfig(const MainAgentConfig& c) {
    std::cout << "provider_kind: " << c.provider_kind << "\n"
              << "base_url:      " << (c.base_url.empty() ? "(default)" : c.base_url) << "\n"
              << "model:         " << c.model << "\n"
              << "api_key_env:   " << (c.api_key_env.empty() ? "(unset)" : c.api_key_env) << "\n"
              << "oauth_file:    " << (c.oauth_file.empty() ? "(unset)" : c.oauth_file) << "\n"
              << "project_id:    " << (c.project_id.empty() ? "(unset)" : c.project_id) << "\n"
              << "location:      " << (c.location.empty() ? "(unset)" : c.location) << "\n"
              << "default_timeout_ms: " << c.default_timeout_ms << "\n";
}

}  // namespace

int RunMainAgentCommand(const MainAgentStore& store, int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }
    const std::string sub = argv[2];

    if (sub == "show") {
        const auto config = store.load();
        if (!config.has_value()) {
            std::cout << "main-agent: not configured (run `agentos main-agent set ...`)\n"
                      << "config_path: " << store.path().string() << "\n";
            return 0;
        }
        PrintConfig(*config);
        std::cout << "config_path: " << store.path().string() << "\n";
        return 0;
    }

    if (sub == "list-providers") {
        std::cout << "openai-chat              POST {base_url}/chat/completions, Bearer auth\n"
                     "anthropic-messages       POST {base_url}/v1/messages, x-api-key header\n"
                     "gemini-generatecontent   POST {base_url}/models/{model}:generateContent, Bearer auth\n"
                     "                         (requires API key from AI Studio; OAuth tokens rejected)\n"
                     "vertex-gemini            POST {base_url}/v1/projects/{project_id}/locations/\n"
                     "                              {location}/publishers/google/models/{model}:generateContent\n"
                     "                         (accepts cloud-platform OAuth tokens; needs project_id+location)\n";
        return 0;
    }

    if (sub == "unset") {
        if (!store.clear()) {
            std::cerr << "main-agent: nothing to clear at " << store.path().string() << "\n";
            return 0;
        }
        std::cout << "main-agent: cleared\n";
        return 0;
    }

    if (sub == "set") {
        const auto opts = ParseKvArgs(argc, argv, 3);
        MainAgentConfig config;
        if (auto it = opts.find("provider"); it != opts.end()) {
            config.provider_kind = it->second;
        } else if (auto it2 = opts.find("provider_kind"); it2 != opts.end()) {
            config.provider_kind = it2->second;
        }
        if (auto it = opts.find("base_url"); it != opts.end()) config.base_url = it->second;
        if (auto it = opts.find("model"); it != opts.end()) config.model = it->second;
        if (auto it = opts.find("api_key_env"); it != opts.end()) config.api_key_env = it->second;
        if (auto it = opts.find("oauth_file"); it != opts.end()) config.oauth_file = it->second;
        if (auto it = opts.find("project_id"); it != opts.end()) config.project_id = it->second;
        if (auto it = opts.find("location"); it != opts.end()) config.location = it->second;
        if (auto it = opts.find("default_timeout_ms"); it != opts.end()) {
            try { config.default_timeout_ms = std::stoi(it->second); } catch (...) {}
        }

        // Validation. We accept config without auth in the file because
        // the env var may legitimately not be set yet at config time —
        // healthy() catches the actual at-request resolution failure.
        std::vector<std::string> missing;
        if (config.provider_kind.empty()) missing.push_back("provider");
        if (config.model.empty()) missing.push_back("model");
        // base_url is optional only for vertex-gemini, where it has a sensible
        // {location}-aiplatform.googleapis.com default. Other providers must set it.
        if (config.base_url.empty() && config.provider_kind != "vertex-gemini") {
            missing.push_back("base_url");
        }
        if (!missing.empty()) {
            std::cerr << "main-agent set: missing required field(s):";
            for (const auto& m : missing) std::cerr << " " << m;
            std::cerr << "\n";
            PrintUsage();
            return 1;
        }
        if (config.provider_kind != "openai-chat" &&
            config.provider_kind != "anthropic-messages" &&
            config.provider_kind != "gemini-generatecontent" &&
            config.provider_kind != "vertex-gemini") {
            std::cerr << "main-agent set: provider must be one of "
                         "openai-chat / anthropic-messages / "
                         "gemini-generatecontent / vertex-gemini "
                         "(got '"
                      << config.provider_kind << "')\n";
            return 1;
        }
        if (config.provider_kind == "vertex-gemini") {
            if (config.project_id.empty() || config.location.empty()) {
                std::cerr << "main-agent set: vertex-gemini requires project_id= and location=\n";
                return 1;
            }
        }
        if (config.api_key_env.empty() && config.oauth_file.empty()) {
            std::cerr << "main-agent set: must specify api_key_env=ENV or oauth_file=path\n";
            return 1;
        }

        if (!store.save(config)) {
            std::cerr << "main-agent set: failed to write " << store.path().string() << "\n";
            return 1;
        }
        std::cout << "main-agent: saved\n";
        PrintConfig(config);
        return 0;
    }

    std::cerr << "main-agent: unknown subcommand '" << sub << "'\n";
    PrintUsage();
    return 1;
}

}  // namespace agentos
