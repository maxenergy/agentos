#pragma once

#include <filesystem>

namespace agentos {

class AuthManager;
class CliHost;
class SecureTokenStore;
class SessionStore;

int RunAuthCommand(
    AuthManager& auth_manager,
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const CliHost& cli_host,
    const std::filesystem::path& workspace,
    int argc,
    char* argv[]);

}  // namespace agentos
