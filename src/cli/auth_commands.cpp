#include "cli/auth_commands.hpp"

#include "auth/auth_manager.hpp"
#include "auth/oauth_pkce.hpp"
#include "auth/secure_token_store.hpp"
#include "auth/session_store.hpp"
#include "hosts/cli/cli_host.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace agentos {

namespace {

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument.rfind("--", 0) == 0) {
            argument = argument.substr(2);
        }

        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        options[argument.substr(0, separator)] = argument.substr(separator + 1);
    }
    return options;
}

std::vector<std::string> SplitCommaList(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

int ParseIntOption(const std::map<std::string, std::string>& options, const std::string& key, const int fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stoi(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

bool ParseBoolOption(const std::map<std::string, std::string>& options, const std::string& key, const bool fallback) {
    if (!options.contains(key)) {
        return fallback;
    }
    const auto value = options.at(key);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

struct BrowserLaunchResult {
    bool attempted = false;
    bool launched = false;
    std::string error;
};

BrowserLaunchResult OpenBrowserUrl(const std::string& url) {
    if (url.empty()) {
        return {
            .attempted = true,
            .launched = false,
            .error = "authorization URL is empty",
        };
    }

#ifdef _WIN32
    const auto result = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    const auto code = reinterpret_cast<std::intptr_t>(result);
    if (code > 32) {
        return {
            .attempted = true,
            .launched = true,
        };
    }
    return {
        .attempted = true,
        .launched = false,
        .error = "ShellExecute failed with code " + std::to_string(code),
    };
#else
    const pid_t child = fork();
    if (child < 0) {
        return {
            .attempted = true,
            .launched = false,
            .error = "failed to fork browser launcher",
        };
    }
    if (child == 0) {
#ifdef __APPLE__
        execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
    return {
        .attempted = true,
        .launched = true,
    };
#endif
}

void PrintAuthUsage() {
    std::cerr
        << "auth commands:\n"
        << "  agentos auth providers\n"
        << "  agentos auth profiles [provider]\n"
        << "  agentos auth credential-store\n"
        << "  agentos auth status [provider] [profile=name]\n"
        << "  agentos auth oauth-defaults [provider]\n"
        << "  agentos auth oauth-config-validate\n"
        << "  agentos auth oauth-start <provider> client_id=ID redirect_uri=URL [authorization_endpoint=URL] [scopes=a,b] [profile=name] [open_browser=true]\n"
        << "  agentos auth oauth-login <provider> client_id=ID redirect_uri=URL [authorization_endpoint=URL] [token_endpoint=URL] [scopes=a,b] [profile=name] [set_default=true] [port=48177] [timeout_ms=120000] [open_browser=true]\n"
        << "  agentos auth oauth-callback callback_url=URL state=STATE\n"
        << "  agentos auth oauth-listen state=STATE port=48177 [timeout_ms=120000]\n"
        << "  agentos auth oauth-complete <provider> callback_url=URL state=STATE code_verifier=VERIFIER redirect_uri=URL client_id=ID [token_endpoint=URL] [profile=name] [account_label=label] [set_default=true]\n"
        << "  agentos auth oauth-token-request token_endpoint=URL client_id=ID redirect_uri=URL code=CODE code_verifier=VERIFIER\n"
        << "  agentos auth oauth-refresh-request token_endpoint=URL client_id=ID refresh_token=TOKEN\n"
        << "  agentos auth login <provider> mode=api-key api_key_env=ENV_NAME [profile=name] [set_default=true]\n"
        << "  agentos auth login <provider> mode=cli-session [profile=name] [set_default=true]\n"
        << "  agentos auth default-profile <provider> profile=name\n"
        << "  agentos auth refresh <provider> [profile=name]\n"
        << "  agentos auth probe <provider>\n"
        << "  agentos auth logout <provider> [profile=name]\n";
}

void PrintAuthSession(const AuthSession& session) {
    std::cout
        << ToString(session.provider)
        << " profile=" << session.profile_name
        << " mode=" << ToString(session.mode)
        << " account=" << session.account_label
        << " source=" << (session.managed_by_external_cli ? "external-cli" : "agentos")
        << '\n';
}

void PrintAuthProfile(const AuthSession& session, const std::string& default_profile) {
    std::cout
        << "auth_profile provider=" << ToString(session.provider)
        << " profile=" << session.profile_name
        << " default=" << (session.profile_name == default_profile ? "true" : "false")
        << " mode=" << ToString(session.mode)
        << " account=\"" << session.account_label << "\""
        << " source=" << (session.managed_by_external_cli ? "external-cli" : "agentos")
        << " refreshable=" << (session.refresh_supported ? "true" : "false")
        << " expired=" << (IsAuthSessionExpired(session) ? "true" : "false")
        << '\n';
}

int PrintAuthProfiles(
    const AuthManager& auth_manager,
    const SessionStore& session_store,
    const int argc,
    char* argv[]) {
    std::optional<AuthProviderId> filter;
    if (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos) {
        const auto provider = ParseAuthProviderId(argv[3]);
        if (!provider.has_value()) {
            std::cerr << "unknown provider: " << argv[3] << '\n';
            return 1;
        }
        filter = *provider;
    }

    for (const auto& session : session_store.list()) {
        if (filter.has_value() && session.provider != *filter) {
            continue;
        }
        PrintAuthProfile(session, auth_manager.default_profile(session.provider));
    }
    return 0;
}

void PrintAuthStatus(const AuthStatus& status) {
    std::cout
        << status.provider_name
        << " profile=" << status.profile_name
        << " authenticated=" << (status.authenticated ? "true" : "false")
        << " expired=" << (status.expired ? "true" : "false")
        << " refreshable=" << (status.refreshable ? "true" : "false")
        << " mode=" << (status.mode.empty() ? "none" : status.mode)
        << " source=" << (status.managed_by_external_cli ? "external-cli" : (status.managed_by_agentos ? "agentos" : "none"))
        << " message=\"" << status.message << "\""
        << '\n';
}

void PrintAuthProviders(const AuthManager& auth_manager) {
    for (const auto& provider : auth_manager.providers()) {
        std::cout
            << provider.provider_name
            << " modes=";

        bool first = true;
        for (const auto mode : provider.supported_modes) {
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << ToString(mode);
        }

        std::cout
            << " browser=" << (provider.browser_login_supported ? "true" : "false")
            << " headless=" << (provider.headless_supported ? "true" : "false")
            << " cli_passthrough=" << (provider.cli_session_passthrough_supported ? "true" : "false")
            << '\n';
    }
}

std::optional<AuthProviderDescriptor> FindAuthProviderDescriptor(
    const AuthManager& auth_manager,
    AuthProviderId provider);

std::filesystem::path OAuthDefaultsConfigPath(const std::filesystem::path& workspace) {
    return workspace / "runtime" / "auth_oauth_providers.tsv";
}

OAuthProviderDefaults EffectiveOAuthDefaultsForProvider(
    const std::filesystem::path& workspace,
    const AuthProviderId provider) {
    const auto builtin = OAuthDefaultsForProvider(provider);
    const auto configured = LoadOAuthProviderDefaultsFromFile(OAuthDefaultsConfigPath(workspace), provider);
    if (!configured.has_value()) {
        return builtin;
    }
    return MergeOAuthProviderDefaults(builtin, *configured);
}

void PrintOAuthDefaultsForProvider(const std::filesystem::path& workspace, const AuthProviderDescriptor& provider) {
    const auto defaults = EffectiveOAuthDefaultsForProvider(workspace, provider.provider);
    std::cout
        << "oauth_defaults provider=" << provider.provider_name
        << " browser=" << (provider.browser_login_supported ? "true" : "false")
        << " supported=" << (defaults.supported ? "true" : "false")
        << " authorization_endpoint=\"" << defaults.authorization_endpoint << "\""
        << " token_endpoint=\"" << defaults.token_endpoint << "\""
        << " scopes=\"";
    for (std::size_t index = 0; index < defaults.scopes.size(); ++index) {
        if (index > 0) {
            std::cout << ',';
        }
        std::cout << defaults.scopes[index];
    }
    std::cout << "\"\n";
}

int PrintOAuthDefaults(const AuthManager& auth_manager, const std::filesystem::path& workspace, const int argc, char* argv[]) {
    if (argc >= 4) {
        const auto provider = ParseAuthProviderId(argv[3]);
        if (!provider.has_value()) {
            std::cerr << "unknown provider: " << argv[3] << '\n';
            return 1;
        }
        const auto descriptor = FindAuthProviderDescriptor(auth_manager, *provider);
        if (!descriptor.has_value()) {
            std::cerr << "provider is not registered: " << argv[3] << '\n';
            return 1;
        }
        const auto defaults = EffectiveOAuthDefaultsForProvider(workspace, *provider);
        PrintOAuthDefaultsForProvider(workspace, *descriptor);
        return defaults.supported ? 0 : 1;
    }

    bool any_supported = false;
    for (const auto& provider : auth_manager.providers()) {
        PrintOAuthDefaultsForProvider(workspace, provider);
        any_supported = EffectiveOAuthDefaultsForProvider(workspace, provider.provider).supported || any_supported;
    }
    return any_supported ? 0 : 1;
}

int ValidateOAuthDefaultsConfig(const AuthManager& auth_manager, const std::filesystem::path& workspace) {
    const auto path = OAuthDefaultsConfigPath(workspace);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cout
            << "oauth_config file=\"" << path.string() << "\""
            << " exists=false valid=true rows=0\n";
        return 0;
    }

    int row_count = 0;
    int diagnostic_count = 0;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        ++row_count;

        const auto fields = SplitTsvFields(line);
        const auto provider = fields.empty() ? std::nullopt : ParseAuthProviderId(fields[0]);
        std::string reason;
        if (fields.size() < 3 || fields.size() > 4) {
            reason = "expected fields: provider, authorization_endpoint, token_endpoint, optional scopes";
        } else if (!provider.has_value()) {
            reason = "unknown provider: " + (fields.empty() ? std::string{} : fields[0]);
        } else if (!FindAuthProviderDescriptor(auth_manager, *provider).has_value()) {
            reason = "provider is not registered: " + fields[0];
        } else if (fields[1].empty()) {
            reason = "authorization_endpoint is required";
        } else if (fields[2].empty()) {
            reason = "token_endpoint is required";
        }

        if (!reason.empty()) {
            ++diagnostic_count;
            std::cout
                << "oauth_config_diagnostic line=" << line_number
                << " reason=\"" << reason << "\"\n";
        }
    }

    std::cout
        << "oauth_config file=\"" << path.string() << "\""
        << " exists=true"
        << " valid=" << (diagnostic_count == 0 ? "true" : "false")
        << " rows=" << row_count
        << " diagnostics=" << diagnostic_count
        << '\n';
    return diagnostic_count == 0 ? 0 : 1;
}

void PrintSecureTokenStoreStatus(const SecureTokenStore& token_store) {
    const auto status = token_store.status();
    std::cout
        << "credential_store backend=" << status.backend_name
        << " system_keychain_backed=" << (status.system_keychain_backed ? "true" : "false")
        << " stores_plaintext=" << (status.stores_plaintext ? "true" : "false")
        << " dev_only=" << (status.dev_only ? "true" : "false")
        << " message=\"" << status.message << "\""
        << '\n';
}

std::optional<AuthProviderDescriptor> FindAuthProviderDescriptor(
    const AuthManager& auth_manager,
    const AuthProviderId provider) {
    const auto providers = auth_manager.providers();
    const auto it = std::find_if(providers.begin(), providers.end(), [&](const AuthProviderDescriptor& descriptor) {
        return descriptor.provider == provider;
    });
    if (it == providers.end()) {
        return std::nullopt;
    }
    return *it;
}

void PrintOAuthPkceStart(const OAuthPkceStart& start) {
    std::cout
        << "oauth_start provider=" << ToString(start.provider)
        << " profile=" << start.profile_name
        << " code_challenge_method=" << start.code_challenge_method
        << " state=" << start.state
        << " code_verifier=" << start.code_verifier
        << " code_challenge=" << start.code_challenge
        << " authorization_url=\"" << start.authorization_url << "\""
        << '\n';
}

void PrintBrowserLaunchResult(const BrowserLaunchResult& result) {
    if (!result.attempted) {
        return;
    }
    std::cout
        << "oauth_browser launched=" << (result.launched ? "true" : "false");
    if (!result.error.empty()) {
        std::cout << " error=\"" << result.error << "\"";
    }
    std::cout << '\n';
}

void PrintOAuthTokenRequest(const OAuthTokenRequest& request) {
    std::cout
        << "oauth_token_request"
        << " token_endpoint=\"" << request.token_endpoint << "\""
        << " content_type=" << request.content_type
        << " body=\"" << request.body << "\""
        << '\n';
}

void PrintOAuthCallbackResult(const OAuthCallbackResult& result) {
    std::cout
        << "oauth_callback success=" << (result.success ? "true" : "false");
    if (result.success) {
        std::cout << " code=" << result.code;
    } else {
        std::cout
            << " error=" << result.error
            << " error_description=\"" << result.error_description << "\"";
    }
    std::cout << '\n';
}


}  // namespace

int RunAuthCommand(AuthManager& auth_manager, SessionStore& session_store, const SecureTokenStore& token_store, const CliHost& cli_host, const std::filesystem::path& workspace, const int argc, char* argv[]) {
    if (argc < 3) {
        PrintAuthUsage();
        return 1;
    }

    const std::string command = argv[2];
    try {
        if (command == "providers") {
            PrintAuthProviders(auth_manager);
            return 0;
        }

        if (command == "profiles") {
            return PrintAuthProfiles(auth_manager, session_store, argc, argv);
        }

        if (command == "credential-store") {
            PrintSecureTokenStoreStatus(token_store);
            return 0;
        }

        if (command == "oauth-defaults") {
            return PrintOAuthDefaults(auth_manager, workspace, argc, argv);
        }

        if (command == "oauth-config-validate") {
            return ValidateOAuthDefaultsConfig(auth_manager, workspace);
        }

        if (command == "oauth-start") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto descriptor = FindAuthProviderDescriptor(auth_manager, *provider);
            if (!descriptor.has_value()) {
                std::cerr << "provider is not registered: " << argv[3] << '\n';
                return 1;
            }
            if (!descriptor->browser_login_supported) {
                std::cerr << "provider does not support browser OAuth: " << argv[3] << '\n';
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto client_id = options.contains("client_id") ? options.at("client_id") : "";
            const auto defaults = EffectiveOAuthDefaultsForProvider(workspace, *provider);
            const auto authorization_endpoint = options.contains("authorization_endpoint")
                ? options.at("authorization_endpoint")
                : (options.contains("auth_endpoint") ? options.at("auth_endpoint") : defaults.authorization_endpoint);
            const auto redirect_uri = options.contains("redirect_uri") ? options.at("redirect_uri") : "";
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);
            const auto scopes = options.contains("scopes") ? SplitCommaList(options.at("scopes")) : defaults.scopes;

            const auto start = CreateOAuthPkceStart(OAuthPkceStartRequest{
                .provider = *provider,
                .profile_name = profile,
                .authorization_endpoint = authorization_endpoint,
                .client_id = client_id,
                .redirect_uri = redirect_uri,
                .scopes = scopes,
            });
            PrintOAuthPkceStart(start);
            if (ParseBoolOption(options, "open_browser", false)) {
                PrintBrowserLaunchResult(OpenBrowserUrl(start.authorization_url));
            }
            return 0;
        }

        if (command == "oauth-login") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto descriptor = FindAuthProviderDescriptor(auth_manager, *provider);
            if (!descriptor.has_value()) {
                std::cerr << "provider is not registered: " << argv[3] << '\n';
                return 1;
            }
            if (!descriptor->browser_login_supported) {
                std::cerr << "provider does not support browser OAuth: " << argv[3] << '\n';
                return 1;
            }

            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto defaults = EffectiveOAuthDefaultsForProvider(workspace, *provider);
            const auto authorization_endpoint = options.contains("authorization_endpoint")
                ? options.at("authorization_endpoint")
                : (options.contains("auth_endpoint") ? options.at("auth_endpoint") : defaults.authorization_endpoint);
            const auto token_endpoint = options.contains("token_endpoint") ? options.at("token_endpoint") : defaults.token_endpoint;
            const auto client_id = options.contains("client_id") ? options.at("client_id") : "";
            const auto redirect_uri = options.contains("redirect_uri") ? options.at("redirect_uri") : "";
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);
            const auto scopes = options.contains("scopes") ? SplitCommaList(options.at("scopes")) : defaults.scopes;
            if (authorization_endpoint.empty() || token_endpoint.empty() || client_id.empty() || redirect_uri.empty()) {
                std::cerr << "authorization_endpoint (or provider default), token_endpoint (or provider default), client_id, and redirect_uri are required\n";
                return 1;
            }

            const auto start = CreateOAuthPkceStart(OAuthPkceStartRequest{
                .provider = *provider,
                .profile_name = profile,
                .authorization_endpoint = authorization_endpoint,
                .client_id = client_id,
                .redirect_uri = redirect_uri,
                .scopes = scopes,
                .state = options.contains("state") ? options.at("state") : "",
                .code_verifier = options.contains("code_verifier") ? options.at("code_verifier") : "",
            });
            PrintOAuthPkceStart(start);
            if (ParseBoolOption(options, "open_browser", false)) {
                PrintBrowserLaunchResult(OpenBrowserUrl(start.authorization_url));
            }

            OAuthCallbackResult callback;
            if (options.contains("callback_url")) {
                callback = ValidateOAuthCallbackUrl(start, options.at("callback_url"));
            } else {
                const auto port = ParseIntOption(options, "port", 0);
                if (port <= 0) {
                    std::cerr << "port is required when callback_url is not provided\n";
                    return 1;
                }
                callback = ListenForOAuthCallbackOnce(OAuthCallbackListenRequest{
                    .start = start,
                    .port = port,
                    .timeout_ms = ParseIntOption(options, "timeout_ms", 120000),
                });
            }
            PrintOAuthCallbackResult(callback);
            if (!callback.success) {
                return 1;
            }

            const auto session = CompleteOAuthLogin(
                cli_host,
                session_store,
                token_store,
                OAuthLoginOrchestrationInput{
                    .start = start,
                    .callback = callback,
                    .token_endpoint = token_endpoint,
                    .client_id = client_id,
                    .account_label = options.contains("account_label") ? options.at("account_label") : "",
                },
                workspace);
            if (ParseBoolOption(options, "set_default", false)) {
                auth_manager.set_default_profile(*provider, session.profile_name);
            }
            PrintAuthSession(session);
            return 0;
        }

        if (command == "oauth-token-request") {
            const auto options = ParseOptionsFromArgs(argc, argv, 3);
            PrintOAuthTokenRequest(BuildOAuthTokenRequest(OAuthTokenRequestInput{
                .token_endpoint = options.contains("token_endpoint") ? options.at("token_endpoint") : "",
                .client_id = options.contains("client_id") ? options.at("client_id") : "",
                .redirect_uri = options.contains("redirect_uri") ? options.at("redirect_uri") : "",
                .code = options.contains("code") ? options.at("code") : "",
                .code_verifier = options.contains("code_verifier") ? options.at("code_verifier") : "",
            }));
            return 0;
        }

        if (command == "oauth-callback") {
            const auto options = ParseOptionsFromArgs(argc, argv, 3);
            const auto callback_url = options.contains("callback_url") ? options.at("callback_url") : "";
            const auto state = options.contains("state") ? options.at("state") : "";
            if (callback_url.empty()) {
                std::cerr << "callback_url is required\n";
                return 1;
            }
            if (state.empty()) {
                std::cerr << "state is required\n";
                return 1;
            }
            PrintOAuthCallbackResult(ValidateOAuthCallbackUrl(OAuthPkceStart{
                .state = state,
            }, callback_url));
            return 0;
        }

        if (command == "oauth-listen") {
            const auto options = ParseOptionsFromArgs(argc, argv, 3);
            const auto state = options.contains("state") ? options.at("state") : "";
            if (state.empty()) {
                std::cerr << "state is required\n";
                return 1;
            }
            const auto port = ParseIntOption(options, "port", 0);
            if (port <= 0) {
                std::cerr << "port is required\n";
                return 1;
            }
            const auto timeout_ms = ParseIntOption(options, "timeout_ms", 120000);
            PrintOAuthCallbackResult(ListenForOAuthCallbackOnce(OAuthCallbackListenRequest{
                .start = OAuthPkceStart{
                    .state = state,
                },
                .port = port,
                .timeout_ms = timeout_ms,
            }));
            return 0;
        }

        if (command == "oauth-complete") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto state = options.contains("state") ? options.at("state") : "";
            const auto callback_url = options.contains("callback_url") ? options.at("callback_url") : "";
            const auto code_verifier = options.contains("code_verifier") ? options.at("code_verifier") : "";
            const auto redirect_uri = options.contains("redirect_uri") ? options.at("redirect_uri") : "";
            const auto defaults = EffectiveOAuthDefaultsForProvider(workspace, *provider);
            const auto token_endpoint = options.contains("token_endpoint") ? options.at("token_endpoint") : defaults.token_endpoint;
            const auto client_id = options.contains("client_id") ? options.at("client_id") : "";
            if (state.empty() || callback_url.empty() || code_verifier.empty() || redirect_uri.empty() ||
                token_endpoint.empty() || client_id.empty()) {
                std::cerr << "callback_url, state, code_verifier, redirect_uri, client_id, and token_endpoint (or provider default) are required\n";
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);
            const auto callback = ValidateOAuthCallbackUrl(OAuthPkceStart{
                .provider = *provider,
                .profile_name = profile,
                .state = state,
                .code_verifier = code_verifier,
                .redirect_uri = redirect_uri,
            }, callback_url);
            if (!callback.success) {
                PrintOAuthCallbackResult(callback);
                return 1;
            }
            const auto session = CompleteOAuthLogin(
                cli_host,
                session_store,
                token_store,
                OAuthLoginOrchestrationInput{
                    .start = OAuthPkceStart{
                        .provider = *provider,
                        .profile_name = profile,
                        .state = state,
                        .code_verifier = code_verifier,
                        .redirect_uri = redirect_uri,
                    },
                    .callback = callback,
                    .token_endpoint = token_endpoint,
                    .client_id = client_id,
                    .account_label = options.contains("account_label") ? options.at("account_label") : "",
                },
                workspace);
            if (ParseBoolOption(options, "set_default", false)) {
                auth_manager.set_default_profile(*provider, session.profile_name);
            }
            PrintAuthSession(session);
            return 0;
        }

        if (command == "oauth-refresh-request") {
            const auto options = ParseOptionsFromArgs(argc, argv, 3);
            PrintOAuthTokenRequest(BuildOAuthRefreshTokenRequest(OAuthRefreshTokenRequestInput{
                .token_endpoint = options.contains("token_endpoint") ? options.at("token_endpoint") : "",
                .client_id = options.contains("client_id") ? options.at("client_id") : "",
                .refresh_token = options.contains("refresh_token") ? options.at("refresh_token") : "",
            }));
            return 0;
        }

        if (command == "status") {
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            if (argc >= 4 && std::string(argv[3]).find('=') == std::string::npos) {
                const auto provider = ParseAuthProviderId(argv[3]);
                if (!provider.has_value()) {
                    std::cerr << "unknown provider: " << argv[3] << '\n';
                    return 1;
                }
                const auto profile = options.contains("profile")
                    ? options.at("profile")
                    : auth_manager.default_profile(*provider);
                PrintAuthStatus(auth_manager.status(*provider, profile));
            } else {
                for (const auto& descriptor : auth_manager.providers()) {
                    const auto profile = options.contains("profile")
                        ? options.at("profile")
                        : auth_manager.default_profile(descriptor.provider);
                    PrintAuthStatus(auth_manager.status(descriptor.provider, profile));
                }
            }
            return 0;
        }

        if (command == "login") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            if (!options.contains("profile")) {
                options["profile"] = auth_manager.default_profile(*provider);
            }

            const auto mode_text = options.contains("mode") ? options["mode"] : "api-key";
            const auto mode = ParseAuthMode(mode_text);
            if (!mode.has_value()) {
                std::cerr << "unknown auth mode: " << mode_text << '\n';
                return 1;
            }

            const auto session = auth_manager.login(*provider, *mode, options);
            if (ParseBoolOption(options, "set_default", false)) {
                auth_manager.set_default_profile(*provider, session.profile_name);
            }
            PrintAuthSession(session);
            return 0;
        }

        if (command == "refresh") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);

            PrintAuthSession(auth_manager.refresh(*provider, profile));
            return 0;
        }

        if (command == "default-profile") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : (argc >= 5 && std::string(argv[4]).find('=') == std::string::npos ? std::string(argv[4]) : "");
            if (profile.empty()) {
                std::cerr << "profile is required\n";
                return 1;
            }

            auth_manager.set_default_profile(*provider, profile);
            std::cout << ToString(*provider) << " default_profile=" << profile << '\n';
            return 0;
        }

        if (command == "probe") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }

            const auto session = auth_manager.probe(*provider);
            if (!session.has_value()) {
                std::cout << ToString(*provider) << " external session: unavailable\n";
                return 1;
            }

            PrintAuthSession(*session);
            return 0;
        }

        if (command == "logout") {
            if (argc < 4) {
                std::cerr << "provider is required\n";
                return 1;
            }
            const auto options = ParseOptionsFromArgs(argc, argv, 4);
            const auto provider = ParseAuthProviderId(argv[3]);
            if (!provider.has_value()) {
                std::cerr << "unknown provider: " << argv[3] << '\n';
                return 1;
            }
            const auto profile = options.contains("profile")
                ? options.at("profile")
                : auth_manager.default_profile(*provider);

            auth_manager.logout(*provider, profile);
            std::cout << "logged out " << ToString(*provider) << " profile=" << profile << '\n';
            return 0;
        }
    } catch (const std::exception& error) {
        std::cerr << "auth error: " << error.what() << '\n';
        return 1;
    }

    PrintAuthUsage();
    return 1;
}


}  // namespace agentos
