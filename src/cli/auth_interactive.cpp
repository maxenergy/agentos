#include "cli/auth_interactive.hpp"

#include <algorithm>
#include <cctype>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ToUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool ReadLine(std::istream& in, std::string& out) {
    if (!std::getline(in, out)) {
        return false;
    }
    if (!out.empty() && out.back() == '\r') {
        out.pop_back();
    }
    return true;
}

// Numeric-fallback line read: empty / non-numeric / out-of-range returns the
// default index (1-based).  EOF returns std::nullopt so the caller can abort.
std::optional<std::size_t> PromptIndexedChoice(
    std::istream& in,
    std::ostream& out,
    const std::string& prompt,
    std::size_t option_count) {
    out << prompt;
    out.flush();

    std::string raw;
    if (!ReadLine(in, raw)) {
        return std::nullopt;
    }
    const auto trimmed = Trim(raw);
    if (trimmed.empty()) {
        return std::size_t{1};
    }
    try {
        const auto value = std::stoul(trimmed);
        if (value >= 1 && value <= option_count) {
            return static_cast<std::size_t>(value);
        }
    } catch (const std::exception&) {
        // fall through to default
    }
    out << "  invalid selection; using default [1]\n";
    return std::size_t{1};
}

std::optional<std::string> PromptLine(
    std::istream& in,
    std::ostream& out,
    const std::string& prompt,
    const std::string& default_value) {
    out << prompt;
    out.flush();
    std::string raw;
    if (!ReadLine(in, raw)) {
        return std::nullopt;
    }
    const auto trimmed = Trim(raw);
    if (trimmed.empty()) {
        return default_value;
    }
    return trimmed;
}

// Y/y/empty -> true ; N/n -> false ; anything else falls back to true after one
// hint (callers explicitly want defaults to win).
std::optional<bool> PromptYesNo(
    std::istream& in,
    std::ostream& out,
    const std::string& prompt,
    bool default_value) {
    out << prompt;
    out.flush();
    std::string raw;
    if (!ReadLine(in, raw)) {
        return std::nullopt;
    }
    const auto trimmed = Trim(raw);
    if (trimmed.empty()) {
        return default_value;
    }
    if (trimmed == "y" || trimmed == "Y" || trimmed == "yes" || trimmed == "Yes") {
        return true;
    }
    if (trimmed == "n" || trimmed == "N" || trimmed == "no" || trimmed == "No") {
        return false;
    }
    out << "  unrecognized answer; using default\n";
    return default_value;
}

// Mode list shown by the prompt.  Ordering mirrors the README examples:
// api-key first (the most common production path), then cli-session for any
// provider whose adapter advertises external-CLI passthrough, then
// browser_oauth when the provider has any OAuth defaults (builtin or config).
struct ModeChoice {
    AuthMode mode;
    std::string label;       // "api-key", "cli-session", "browser_oauth"
    std::string description; // shown in the menu
};

std::vector<ModeChoice> ModesForProvider(
    const AuthProviderDescriptor& descriptor,
    const OAuthProviderDefaults& defaults) {
    std::vector<ModeChoice> modes;

    const auto supports = [&](AuthMode candidate) {
        return std::find(descriptor.supported_modes.begin(), descriptor.supported_modes.end(), candidate)
            != descriptor.supported_modes.end();
    };

    if (supports(AuthMode::api_key)) {
        modes.push_back({
            .mode = AuthMode::api_key,
            .label = "api-key",
            .description = "API key from an environment variable (recommended for production)",
        });
    }
    if (supports(AuthMode::cli_session_passthrough) || descriptor.cli_session_passthrough_supported) {
        modes.push_back({
            .mode = AuthMode::cli_session_passthrough,
            .label = "cli-session",
            .description = "Reuse an existing external CLI session (e.g. claude/codex/gemini login)",
        });
    }
    // browser_oauth is shown whenever the provider exposes any OAuth metadata
    // (builtin defaults OR a workspace override).  For stub providers the
    // option is still surfaced because the user may have configured endpoints
    // in runtime/auth_oauth_providers.tsv that the descriptor does not yet
    // know about — the dispatcher routes them to `auth oauth-login` either
    // way and that command will fail loudly if endpoints are missing.
    if (supports(AuthMode::browser_oauth) || defaults.supported || defaults.origin == "config") {
        modes.push_back({
            .mode = AuthMode::browser_oauth,
            .label = "browser_oauth",
            .description = defaults.supported
                ? "Browser OAuth (PKCE) — opens the provider's authorization page"
                : "Browser OAuth (PKCE) — requires endpoints in runtime/auth_oauth_providers.tsv",
        });
    }
    return modes;
}

}  // namespace

std::string DefaultApiKeyEnvName(const AuthProviderId provider) {
    return "AGENTOS_" + ToUpper(ToString(provider)) + "_API_KEY";
}

InteractiveLoginResolution PromptInteractiveLogin(
    const std::vector<AuthProviderDescriptor>& providers,
    const std::function<OAuthProviderDefaults(AuthProviderId)>& defaults_fn,
    const std::optional<AuthProviderId> preselected_provider,
    std::istream& in,
    std::ostream& out) {
    InteractiveLoginResolution resolution;

    if (providers.empty()) {
        resolution.error_message = "no providers are registered";
        return resolution;
    }

    // ---- Step 1: provider selection ----
    AuthProviderDescriptor descriptor;
    if (preselected_provider.has_value()) {
        const auto it = std::find_if(providers.begin(), providers.end(), [&](const AuthProviderDescriptor& d) {
            return d.provider == *preselected_provider;
        });
        if (it == providers.end()) {
            resolution.error_message = "provider is not registered: " + ToString(*preselected_provider);
            return resolution;
        }
        descriptor = *it;
        out << "provider: " << descriptor.provider_name << " (preselected)\n";
    } else {
        out << "Available providers:\n";
        for (std::size_t index = 0; index < providers.size(); ++index) {
            out << "  " << (index + 1) << ") " << providers[index].provider_name << '\n';
        }
        const auto choice = PromptIndexedChoice(in, out, "Choose a provider [1]: ", providers.size());
        if (!choice.has_value()) {
            resolution.error_message = "stdin closed before provider selection";
            return resolution;
        }
        descriptor = providers[*choice - 1];
    }
    resolution.provider = descriptor.provider;

    // ---- Step 2: surface OAuth defaults metadata ----
    const auto defaults = defaults_fn(descriptor.provider);
    const std::string origin = defaults.origin.empty()
        ? (defaults.supported ? std::string("builtin") : std::string("none"))
        : defaults.origin;
    out << "  oauth_defaults origin=" << origin
        << " supported=" << (defaults.supported ? "true" : "false") << '\n';
    if (!defaults.note.empty()) {
        out << "  note: " << defaults.note << '\n';
    }

    // ---- Step 3: mode selection ----
    const auto modes = ModesForProvider(descriptor, defaults);
    if (modes.empty()) {
        resolution.error_message = "provider " + descriptor.provider_name +
            " has no supported login modes";
        return resolution;
    }

    out << "Available modes for " << descriptor.provider_name << ":\n";
    for (std::size_t index = 0; index < modes.size(); ++index) {
        out << "  " << (index + 1) << ") " << modes[index].label
            << " - " << modes[index].description << '\n';
    }
    const auto mode_choice = PromptIndexedChoice(in, out, "Choose mode [1]: ", modes.size());
    if (!mode_choice.has_value()) {
        resolution.error_message = "stdin closed before mode selection";
        return resolution;
    }
    const auto& chosen_mode = modes[*mode_choice - 1];
    resolution.mode = chosen_mode.mode;

    // ---- Step 4: mode-specific follow-up ----
    if (chosen_mode.mode == AuthMode::browser_oauth) {
        resolution.redirect_to_oauth_login = true;
        std::ostringstream message;
        message << "interactive login does not embed PKCE; use `agentos auth oauth-login "
                << ToString(descriptor.provider) << "` for the OAuth orchestrator";
        if (!defaults.supported && defaults.origin != "config") {
            message << " (provider currently registered as `" << origin
                    << "`; configure endpoints in runtime/auth_oauth_providers.tsv first)";
        } else if (defaults.origin == "config") {
            message << " (uses workspace overrides from runtime/auth_oauth_providers.tsv)";
        }
        message << "; example: agentos auth oauth-login " << ToString(descriptor.provider)
                << " client_id=<CLIENT_ID> redirect_uri=http://127.0.0.1:48177/callback"
                << " port=48177 open_browser=true profile=default set_default=true";
        resolution.error_message = message.str();
        return resolution;
    }

    if (chosen_mode.mode == AuthMode::api_key) {
        const auto env_default = DefaultApiKeyEnvName(descriptor.provider);
        const auto env_name = PromptLine(in, out,
            "API key environment variable name [" + env_default + "]: ",
            env_default);
        if (!env_name.has_value()) {
            resolution.error_message = "stdin closed before API key env name";
            return resolution;
        }
        resolution.api_key_env = *env_name;
    }

    const auto profile = PromptLine(in, out, "Profile name [default]: ", "default");
    if (!profile.has_value()) {
        resolution.error_message = "stdin closed before profile name";
        return resolution;
    }
    resolution.profile_name = *profile;

    const auto set_default = PromptYesNo(in, out, "Set as default profile? [Y/n]: ", true);
    if (!set_default.has_value()) {
        resolution.error_message = "stdin closed before set-default confirmation";
        return resolution;
    }
    resolution.set_default = *set_default;
    resolution.ok = true;
    return resolution;
}

std::vector<std::string> BuildLoginArgvFromResolution(const InteractiveLoginResolution& resolution) {
    std::vector<std::string> argv;
    argv.reserve(7);
    argv.emplace_back("agentos");
    argv.emplace_back("auth");
    argv.emplace_back("login");
    argv.emplace_back(ToString(resolution.provider));

    // The existing `auth login` handler accepts `mode=api-key` or
    // `mode=cli-session`; ParseAuthMode normalizes hyphens to underscores so
    // either spelling round-trips, but we follow the README form.
    if (resolution.mode == AuthMode::api_key) {
        argv.emplace_back("mode=api-key");
        argv.emplace_back("api_key_env=" + resolution.api_key_env);
    } else if (resolution.mode == AuthMode::cli_session_passthrough) {
        argv.emplace_back("mode=cli-session");
    } else {
        argv.emplace_back("mode=" + ToString(resolution.mode));
    }
    argv.emplace_back("profile=" + resolution.profile_name);
    argv.emplace_back(std::string("set_default=") + (resolution.set_default ? "true" : "false"));
    return argv;
}

}  // namespace agentos
