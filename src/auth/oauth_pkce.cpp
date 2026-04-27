#include "auth/oauth_pkce.hpp"

#include "utils/curl_secret.hpp"
#include "utils/secure_random.hpp"
#include "utils/spec_parsing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace agentos {

namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t RotateRight(const std::uint32_t value, const int bits) {
    return (value >> bits) | (value << (32 - bits));
}

std::array<std::uint8_t, 32> Sha256(const std::string& input) {
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) {
        data.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    std::array<std::uint32_t, 8> hash{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    for (std::size_t chunk = 0; chunk < data.size(); chunk += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const auto offset = chunk + i * 4U;
            words[i] = (static_cast<std::uint32_t>(data[offset]) << 24U) |
                       (static_cast<std::uint32_t>(data[offset + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(data[offset + 2U]) << 8U) |
                       static_cast<std::uint32_t>(data[offset + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const auto s0 = RotateRight(words[i - 15U], 7) ^ RotateRight(words[i - 15U], 18) ^ (words[i - 15U] >> 3U);
            const auto s1 = RotateRight(words[i - 2U], 17) ^ RotateRight(words[i - 2U], 19) ^ (words[i - 2U] >> 10U);
            words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
        }

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t i = 0; i < 64U; ++i) {
            const auto s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + kSha256RoundConstants[i] + words[i];
            const auto s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>((hash[i] >> 24U) & 0xffU);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>((hash[i] >> 16U) & 0xffU);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>((hash[i] >> 8U) & 0xffU);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(hash[i] & 0xffU);
    }
    return digest;
}

std::string Base64UrlEncode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const auto byte : bytes) {
        buffer = (buffer << 8U) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(kAlphabet[(buffer >> bits) & 0x3fU]);
        }
    }
    if (bits > 0) {
        output.push_back(kAlphabet[(buffer << (6 - bits)) & 0x3fU]);
    }
    return output;
}

std::string RandomBase64Url(const std::size_t byte_count) {
    return Base64UrlEncode(SecureRandomBytes(byte_count));
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream output;
    output << std::hex << std::uppercase;
    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output << ch;
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return output.str();
}

int HexValue(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string& value) {
    std::string output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch == '+') {
            output.push_back(' ');
            continue;
        }
        if (ch == '%' && index + 2 < value.size()) {
            const auto high = HexValue(value[index + 1]);
            const auto low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(ch);
    }
    return output;
}

std::map<std::string, std::string> ParseQueryParams(const std::string& query) {
    std::map<std::string, std::string> params;
    std::size_t start = 0;
    while (start <= query.size()) {
        const auto end = query.find('&', start);
        const auto part = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            const auto equals = part.find('=');
            if (equals == std::string::npos) {
                params[UrlDecode(part)] = "";
            } else {
                params[UrlDecode(part.substr(0, equals))] = UrlDecode(part.substr(equals + 1));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return params;
}

std::string ExtractQueryString(const std::string& callback_url) {
    const auto question = callback_url.find('?');
    if (question == std::string::npos) {
        return {};
    }
    const auto fragment = callback_url.find('#', question + 1);
    return callback_url.substr(
        question + 1,
        fragment == std::string::npos ? std::string::npos : fragment - question - 1);
}

std::optional<std::string> ExtractHttpRequestTarget(const std::string& request_text) {
    const auto line_end = request_text.find("\r\n");
    const auto request_line = request_text.substr(0, line_end == std::string::npos ? request_text.find('\n') : line_end);
    const auto first_space = request_line.find(' ');
    if (first_space == std::string::npos) {
        return std::nullopt;
    }
    const auto second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string::npos || second_space <= first_space + 1) {
        return std::nullopt;
    }
    if (request_line.substr(0, first_space) != "GET") {
        return std::nullopt;
    }
    return request_line.substr(first_space + 1, second_space - first_space - 1);
}

std::string ExtractRedirectUriPath(const std::string& redirect_uri) {
    const auto scheme_end = redirect_uri.find("://");
    if (scheme_end == std::string::npos) {
        return "/";
    }
    const auto authority_start = scheme_end + 3;
    const auto path_start = redirect_uri.find('/', authority_start);
    if (path_start == std::string::npos) {
        return "/";
    }
    const auto query_start = redirect_uri.find_first_of("?#", path_start);
    auto path = query_start == std::string::npos
        ? redirect_uri.substr(path_start)
        : redirect_uri.substr(path_start, query_start - path_start);
    if (path.empty()) {
        return "/";
    }
    return path;
}

std::string TargetPathOnly(const std::string& target) {
    const auto query = target.find_first_of("?#");
    return query == std::string::npos ? target : target.substr(0, query);
}

std::optional<std::string> ExtractHttpHostHeader(const std::string& request_text) {
    std::size_t cursor = request_text.find("\r\n");
    if (cursor == std::string::npos) {
        return std::nullopt;
    }
    cursor += 2;
    while (cursor < request_text.size()) {
        const auto line_end = request_text.find("\r\n", cursor);
        const auto line = request_text.substr(
            cursor,
            line_end == std::string::npos ? std::string::npos : line_end - cursor);
        if (line.empty()) {
            return std::nullopt;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (name == "host") {
                std::size_t value_begin = colon + 1;
                while (value_begin < line.size() &&
                       (line[value_begin] == ' ' || line[value_begin] == '\t')) {
                    ++value_begin;
                }
                std::size_t value_end = line.size();
                while (value_end > value_begin &&
                       (line[value_end - 1] == ' ' || line[value_end - 1] == '\t')) {
                    --value_end;
                }
                return line.substr(value_begin, value_end - value_begin);
            }
        }
        if (line_end == std::string::npos) {
            return std::nullopt;
        }
        cursor = line_end + 2;
    }
    return std::nullopt;
}

bool HostHeaderMatchesLoopback(const std::string& host_header, int port) {
    if (host_header.empty()) {
        return false;
    }
    const auto port_str = ":" + std::to_string(port);
    return host_header == "127.0.0.1" + port_str ||
           host_header == "localhost" + port_str;
}

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void CloseSocket(const SocketHandle socket_handle) {
    closesocket(socket_handle);
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockSession() {
        if (ok_) {
            WSACleanup();
        }
    }
    bool ok() const {
        return ok_;
    }

private:
    bool ok_ = false;
};
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void CloseSocket(const SocketHandle socket_handle) {
    close(socket_handle);
}
#endif

OAuthCallbackResult CallbackListenError(const std::string& error, const std::string& description) {
    return {
        .success = false,
        .error = error,
        .error_description = description,
    };
}

void SendHttpResponse(const SocketHandle client_socket, const OAuthCallbackResult& result) {
    const auto body = result.success
        ? std::string("<html><body>AgentOS OAuth callback received.</body></html>")
        : std::string("<html><body>AgentOS OAuth callback failed: ") + result.error + "</body></html>";
    const auto status = result.success ? "200 OK" : "400 Bad Request";
    const auto response =
        "HTTP/1.1 " + std::string(status) + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    (void)send(client_socket, response.c_str(), static_cast<int>(response.size()), 0);
}

std::string JoinScopes(const std::vector<std::string>& scopes) {
    std::string joined;
    for (const auto& scope : scopes) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        joined += scope;
    }
    return joined;
}

void AppendQueryParam(std::string& url, const std::string& key, const std::string& value) {
    url.push_back(url.find('?') == std::string::npos ? '?' : '&');
    url += UrlEncode(key);
    url.push_back('=');
    url += UrlEncode(value);
}

void AppendFormParam(std::string& body, const std::string& key, const std::string& value) {
    if (!body.empty()) {
        body.push_back('&');
    }
    body += UrlEncode(key);
    body.push_back('=');
    body += UrlEncode(value);
}

// ADR-JSON-001 phase 2: parse the OAuth token response with nlohmann/json so
// whitespace, key order, unicode-escaped values, and nested objects all
// round-trip correctly.  Returns nullopt on parse failure or when the field is
// absent / wrong-typed, matching the previous hand-rolled contract.
const nlohmann::json* TopLevelJsonField(const nlohmann::json& root, const std::string& key) {
    if (!root.is_object()) {
        return nullptr;
    }
    const auto it = root.find(key);
    if (it == root.end()) {
        return nullptr;
    }
    return &(*it);
}

std::optional<nlohmann::json> ParseTokenResponseJson(const std::string& text) {
    try {
        return nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> JsonStringField(const std::string& text, const std::string& key) {
    const auto root = ParseTokenResponseJson(text);
    if (!root.has_value()) {
        return std::nullopt;
    }
    const auto* value = TopLevelJsonField(*root, key);
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

std::optional<int> JsonIntField(const std::string& text, const std::string& key) {
    const auto root = ParseTokenResponseJson(text);
    if (!root.has_value()) {
        return std::nullopt;
    }
    const auto* value = TopLevelJsonField(*root, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_number_integer()) {
        return value->get<int>();
    }
    if (value->is_number_float()) {
        return static_cast<int>(value->get<double>());
    }
    if (value->is_string()) {
        try {
            return std::stoi(value->get<std::string>());
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::chrono::system_clock::time_point ExpiryFromTokenResponse(const OAuthTokenResponse& response) {
    if (response.expires_in_seconds <= 0) {
        return std::chrono::system_clock::time_point::max();
    }
    return std::chrono::system_clock::now() + std::chrono::seconds(response.expires_in_seconds);
}

void RequireSuccessfulTokenResponse(const OAuthTokenResponse& response) {
    if (!response.success) {
        throw std::runtime_error(response.error.empty() ? "OAuthTokenResponseFailed" : response.error);
    }
    if (response.access_token.empty()) {
        throw std::runtime_error("MissingAccessToken");
    }
}

}  // namespace

std::string CreatePkceCodeChallengeForTest(const std::string& verifier) {
    const auto digest = Sha256(verifier);
    return Base64UrlEncode(std::vector<std::uint8_t>(digest.begin(), digest.end()));
}

std::string ExtractRedirectUriPathForTest(const std::string& redirect_uri) {
    return ExtractRedirectUriPath(redirect_uri);
}

std::string TargetPathOnlyForTest(const std::string& target) {
    return TargetPathOnly(target);
}

std::optional<std::string> ExtractHttpHostHeaderForTest(const std::string& request_text) {
    return ExtractHttpHostHeader(request_text);
}

bool HostHeaderMatchesLoopbackForTest(const std::string& host_header, int port) {
    return HostHeaderMatchesLoopback(host_header, port);
}

OAuthProviderDefaults OAuthDefaultsForProvider(const AuthProviderId provider) {
    if (provider == AuthProviderId::gemini) {
        return {
            .supported = true,
            .authorization_endpoint = "https://accounts.google.com/o/oauth2/v2/auth",
            .token_endpoint = "https://oauth2.googleapis.com/token",
            .scopes = {
                "openid",
                "email",
                "profile",
                "https://www.googleapis.com/auth/cloud-platform",
            },
            .origin = "builtin",
            .note = "Google OAuth 2.0 PKCE endpoints (shared with the Gemini CLI).",
        };
    }
    if (provider == AuthProviderId::openai) {
        // OpenAI / Codex CLI use a non-public OAuth flow today; we surface a
        // stub entry so `auth oauth-defaults` lists the provider and users can
        // override via runtime/auth_oauth_providers.tsv when they have valid
        // endpoints.
        return {
            .supported = false,
            .authorization_endpoint = "",
            .token_endpoint = "",
            .scopes = {},
            .origin = "stub",
            .note = "OpenAI/Codex public PKCE endpoints are not yet documented; supply "
                    "authorization_endpoint and token_endpoint via runtime/auth_oauth_providers.tsv "
                    "or the oauth-start/oauth-login CLI flags.",
        };
    }
    if (provider == AuthProviderId::anthropic) {
        return {
            .supported = false,
            .authorization_endpoint = "",
            .token_endpoint = "",
            .scopes = {},
            .origin = "stub",
            .note = "Anthropic/Claude public PKCE endpoints are not yet documented; supply "
                    "authorization_endpoint and token_endpoint via runtime/auth_oauth_providers.tsv "
                    "or the oauth-start/oauth-login CLI flags.",
        };
    }
    if (provider == AuthProviderId::qwen) {
        return {
            .supported = false,
            .authorization_endpoint = "",
            .token_endpoint = "",
            .scopes = {},
            .origin = "stub",
            .note = "Alibaba Qwen API uses long-lived API keys; OAuth PKCE is not currently "
                    "applicable. Use mode=api-key for production setups.",
        };
    }
    return {};
}

OAuthProviderDefaults MergeOAuthProviderDefaults(
    const OAuthProviderDefaults& base,
    const OAuthProviderDefaults& override_defaults) {
    auto merged = base;
    bool overridden = false;
    if (!override_defaults.authorization_endpoint.empty()) {
        merged.authorization_endpoint = override_defaults.authorization_endpoint;
        overridden = true;
    }
    if (!override_defaults.token_endpoint.empty()) {
        merged.token_endpoint = override_defaults.token_endpoint;
        overridden = true;
    }
    if (!override_defaults.scopes.empty()) {
        merged.scopes = override_defaults.scopes;
        overridden = true;
    }
    merged.supported = !merged.authorization_endpoint.empty() && !merged.token_endpoint.empty();
    if (overridden) {
        merged.origin = "config";
        // Preserve the base note unless it was the stub note for an
        // unsupported builtin entry — config typically resolves the gap.
        if (merged.supported && base.origin == "stub") {
            merged.note = "Endpoints supplied by runtime/auth_oauth_providers.tsv override.";
        }
    }
    return merged;
}

std::map<AuthProviderId, OAuthProviderDefaults> LoadOAuthProviderDefaultsFromFile(
    const std::filesystem::path& path) {
    std::map<AuthProviderId, OAuthProviderDefaults> defaults_by_provider;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return defaults_by_provider;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto fields = SplitTsvFields(line);
        if (fields.empty()) {
            continue;
        }
        const auto provider = ParseAuthProviderId(fields[0]);
        if (!provider.has_value()) {
            continue;
        }

        OAuthProviderDefaults defaults;
        defaults.authorization_endpoint = fields.size() > 1 ? fields[1] : "";
        defaults.token_endpoint = fields.size() > 2 ? fields[2] : "";
        defaults.scopes = fields.size() > 3 ? SplitNonEmpty(fields[3], ',') : std::vector<std::string>{};
        defaults.supported = !defaults.authorization_endpoint.empty() && !defaults.token_endpoint.empty();
        defaults.origin = "config";
        defaults_by_provider[*provider] = std::move(defaults);
    }

    return defaults_by_provider;
}

std::optional<OAuthProviderDefaults> LoadOAuthProviderDefaultsFromFile(
    const std::filesystem::path& path,
    const AuthProviderId provider) {
    const auto defaults_by_provider = LoadOAuthProviderDefaultsFromFile(path);
    const auto it = defaults_by_provider.find(provider);
    if (it == defaults_by_provider.end()) {
        return std::nullopt;
    }
    return it->second;
}

OAuthPkceStart CreateOAuthPkceStart(OAuthPkceStartRequest request) {
    const auto defaults = OAuthDefaultsForProvider(request.provider);
    if (request.authorization_endpoint.empty()) {
        request.authorization_endpoint = defaults.authorization_endpoint;
    }
    if (request.scopes.empty()) {
        request.scopes = defaults.scopes;
    }
    if (request.authorization_endpoint.empty()) {
        throw std::invalid_argument("authorization_endpoint is required");
    }
    if (request.client_id.empty()) {
        throw std::invalid_argument("client_id is required");
    }
    if (request.redirect_uri.empty()) {
        throw std::invalid_argument("redirect_uri is required");
    }

    OAuthPkceStart start{
        .provider = request.provider,
        .profile_name = request.profile_name.empty() ? "default" : request.profile_name,
        .state = request.state.empty() ? RandomBase64Url(32) : request.state,
        .code_verifier = request.code_verifier.empty() ? RandomBase64Url(48) : request.code_verifier,
        .redirect_uri = request.redirect_uri,
    };
    start.code_challenge = CreatePkceCodeChallengeForTest(start.code_verifier);
    start.authorization_url = request.authorization_endpoint;
    AppendQueryParam(start.authorization_url, "response_type", "code");
    AppendQueryParam(start.authorization_url, "client_id", request.client_id);
    AppendQueryParam(start.authorization_url, "redirect_uri", request.redirect_uri);
    if (!request.scopes.empty()) {
        AppendQueryParam(start.authorization_url, "scope", JoinScopes(request.scopes));
    }
    AppendQueryParam(start.authorization_url, "state", start.state);
    AppendQueryParam(start.authorization_url, "code_challenge", start.code_challenge);
    AppendQueryParam(start.authorization_url, "code_challenge_method", start.code_challenge_method);
    return start;
}

OAuthCallbackResult ValidateOAuthCallback(
    const OAuthPkceStart& start,
    const std::string& returned_state,
    const std::string& code,
    const std::string& error,
    const std::string& error_description) {
    if (!error.empty()) {
        return {
            .success = false,
            .error = error,
            .error_description = error_description,
        };
    }
    if (returned_state.empty() || returned_state != start.state) {
        return {
            .success = false,
            .error = "InvalidOAuthState",
            .error_description = "OAuth callback state did not match the pending login request",
        };
    }
    if (code.empty()) {
        return {
            .success = false,
            .error = "MissingOAuthCode",
            .error_description = "OAuth callback did not include an authorization code",
        };
    }
    return {
        .success = true,
        .code = code,
    };
}

OAuthCallbackResult ValidateOAuthCallbackUrl(
    const OAuthPkceStart& start,
    const std::string& callback_url) {
    const auto params = ParseQueryParams(ExtractQueryString(callback_url));
    const auto state_it = params.find("state");
    const auto code_it = params.find("code");
    const auto error_it = params.find("error");
    const auto error_description_it = params.find("error_description");
    return ValidateOAuthCallback(
        start,
        state_it == params.end() ? "" : state_it->second,
        code_it == params.end() ? "" : code_it->second,
        error_it == params.end() ? "" : error_it->second,
        error_description_it == params.end() ? "" : error_description_it->second);
}

OAuthCallbackResult ListenForOAuthCallbackOnce(const OAuthCallbackListenRequest& request) {
    if (request.port <= 0 || request.port > 65535) {
        return CallbackListenError("InvalidCallbackPort", "OAuth callback listener requires a valid TCP port");
    }
#ifdef _WIN32
    WinsockSession winsock;
    if (!winsock.ok()) {
        return CallbackListenError("CallbackListenerUnavailable", "failed to initialize Winsock");
    }
#endif

    const SocketHandle server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == kInvalidSocket) {
        return CallbackListenError("CallbackListenerUnavailable", "failed to create listener socket");
    }

    int reuse = 1;
    (void)setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(request.port));
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        CloseSocket(server_socket);
        return CallbackListenError("CallbackListenerUnavailable", "failed to configure loopback address");
    }

    if (bind(server_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CloseSocket(server_socket);
        return CallbackListenError("CallbackListenerUnavailable", "failed to bind callback listener");
    }
    if (listen(server_socket, 1) != 0) {
        CloseSocket(server_socket);
        return CallbackListenError("CallbackListenerUnavailable", "failed to listen for callback");
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(server_socket, &read_set);
    timeval timeout{};
    timeout.tv_sec = request.timeout_ms / 1000;
    timeout.tv_usec = (request.timeout_ms % 1000) * 1000;
    const auto ready = select(static_cast<int>(server_socket + 1), &read_set, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        CloseSocket(server_socket);
        return CallbackListenError("CallbackTimeout", "timed out waiting for OAuth callback");
    }

    const SocketHandle client_socket = accept(server_socket, nullptr, nullptr);
    CloseSocket(server_socket);
    if (client_socket == kInvalidSocket) {
        return CallbackListenError("CallbackListenerUnavailable", "failed to accept OAuth callback");
    }

    std::string request_text(4096, '\0');
    const auto received = recv(client_socket, request_text.data(), static_cast<int>(request_text.size()), 0);
    if (received <= 0) {
        CloseSocket(client_socket);
        return CallbackListenError("InvalidCallbackRequest", "OAuth callback request was empty");
    }
    request_text.resize(static_cast<std::size_t>(received));
    const auto target = ExtractHttpRequestTarget(request_text);
    if (!target.has_value()) {
        const auto result = CallbackListenError("InvalidCallbackRequest", "OAuth callback request was not a GET request");
        SendHttpResponse(client_socket, result);
        CloseSocket(client_socket);
        return result;
    }

    const auto expected_path = ExtractRedirectUriPath(request.start.redirect_uri);
    if (TargetPathOnly(*target) != expected_path) {
        const auto result = CallbackListenError(
            "InvalidCallbackRequest",
            "OAuth callback request path did not match the registered redirect_uri path");
        SendHttpResponse(client_socket, result);
        CloseSocket(client_socket);
        return result;
    }

    const auto host_header = ExtractHttpHostHeader(request_text);
    if (!host_header.has_value() || !HostHeaderMatchesLoopback(*host_header, request.port)) {
        const auto result = CallbackListenError(
            "InvalidCallbackRequest",
            "OAuth callback Host header must reference the loopback listener");
        SendHttpResponse(client_socket, result);
        CloseSocket(client_socket);
        return result;
    }

    const auto callback_url = "http://127.0.0.1:" + std::to_string(request.port) + *target;
    const auto result = ValidateOAuthCallbackUrl(request.start, callback_url);
    SendHttpResponse(client_socket, result);
    CloseSocket(client_socket);
    return result;
}

OAuthTokenRequest BuildOAuthTokenRequest(const OAuthTokenRequestInput& input) {
    if (input.token_endpoint.empty()) {
        throw std::invalid_argument("token_endpoint is required");
    }
    if (input.client_id.empty()) {
        throw std::invalid_argument("client_id is required");
    }
    if (input.redirect_uri.empty()) {
        throw std::invalid_argument("redirect_uri is required");
    }
    if (input.code.empty()) {
        throw std::invalid_argument("code is required");
    }
    if (input.code_verifier.empty()) {
        throw std::invalid_argument("code_verifier is required");
    }

    OAuthTokenRequest request{
        .token_endpoint = input.token_endpoint,
    };
    AppendFormParam(request.body, "grant_type", "authorization_code");
    AppendFormParam(request.body, "code", input.code);
    AppendFormParam(request.body, "client_id", input.client_id);
    AppendFormParam(request.body, "redirect_uri", input.redirect_uri);
    AppendFormParam(request.body, "code_verifier", input.code_verifier);
    return request;
}

OAuthTokenRequest BuildOAuthRefreshTokenRequest(const OAuthRefreshTokenRequestInput& input) {
    if (input.token_endpoint.empty()) {
        throw std::invalid_argument("token_endpoint is required");
    }
    if (input.client_id.empty()) {
        throw std::invalid_argument("client_id is required");
    }
    if (input.refresh_token.empty()) {
        throw std::invalid_argument("refresh_token is required");
    }

    OAuthTokenRequest request{
        .token_endpoint = input.token_endpoint,
    };
    AppendFormParam(request.body, "grant_type", "refresh_token");
    AppendFormParam(request.body, "refresh_token", input.refresh_token);
    AppendFormParam(request.body, "client_id", input.client_id);
    return request;
}

OAuthTokenResponse ParseOAuthTokenResponse(const std::string& response_json) {
    const auto error = JsonStringField(response_json, "error");
    if (error.has_value() && !error->empty()) {
        return {
            .success = false,
            .error = *error,
            .error_description = JsonStringField(response_json, "error_description").value_or(""),
        };
    }

    const auto access_token = JsonStringField(response_json, "access_token");
    if (!access_token.has_value() || access_token->empty()) {
        return {
            .success = false,
            .error = "MissingAccessToken",
            .error_description = "OAuth token response did not include access_token",
        };
    }

    return {
        .success = true,
        .access_token = *access_token,
        .refresh_token = JsonStringField(response_json, "refresh_token").value_or(""),
        .token_type = JsonStringField(response_json, "token_type").value_or("Bearer"),
        .expires_in_seconds = JsonIntField(response_json, "expires_in").value_or(0),
    };
}

OAuthTokenResponse ExecuteOAuthTokenExchange(
    const CliHost& cli_host,
    const OAuthTokenRequestInput& input,
    const std::filesystem::path& workspace_path,
    const int timeout_ms) {
    const auto token_request = BuildOAuthTokenRequest(input);

    // The form-encoded body contains `code_verifier`, `code`, and (for some
    // providers) `client_secret`. Passing it as `--data <body>` would expose
    // those values in the curl process's argv. Spool the body to a short-lived
    // file under runtime/.tmp/ and reference it via `--data @<file>`.
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(workspace_path, token_request.body, /*header_lines=*/{});
    } catch (const std::exception& error) {
        return {
            .success = false,
            .error = "OAuthTokenExchangeFailed",
            .error_description = std::string("could not stage OAuth token request body: ") + error.what(),
        };
    }
    const auto body_arg = std::string("@") + secret_files.body_file.string();

    const CliSpec spec{
        .name = "oauth_token_exchange",
        .description = "Exchange an OAuth authorization code for tokens through curl.",
        .binary = "curl",
        .args_template = {
            "-sS",
            "-X",
            "POST",
            "-H",
            "Content-Type: {{content_type}}",
            "--data",
            "{{token_request_body_file}}",
            "{{oauth_token_endpoint}}",
        },
        .required_args = {"oauth_token_endpoint", "content_type", "token_request_body_file"},
        .input_schema_json = R"({"type":"object"})",
        .output_schema_json = R"({"type":"object"})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = timeout_ms,
        .output_limit_bytes = 64 * 1024,
        .env_allowlist = {"PATH"},
    };

    const auto result = cli_host.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"oauth_token_endpoint", token_request.token_endpoint},
            {"content_type", token_request.content_type},
            {"token_request_body_file", body_arg},
        },
        .workspace_path = workspace_path,
    });
    if (!result.success) {
        return {
            .success = false,
            .error = result.error_code.empty() ? "OAuthTokenExchangeFailed" : result.error_code,
            .error_description = result.error_message.empty() ? result.stderr_text : result.error_message,
        };
    }
    return ParseOAuthTokenResponse(result.stdout_text);
}

OAuthTokenResponse ExecuteOAuthRefreshTokenExchange(
    const CliHost& cli_host,
    const OAuthRefreshTokenRequestInput& input,
    const std::filesystem::path& workspace_path,
    const int timeout_ms) {
    const auto token_request = BuildOAuthRefreshTokenRequest(input);

    // The refresh form body contains the long-lived `refresh_token` (and
    // sometimes `client_secret`). Spool to a temp file so curl never sees it
    // on argv.
    CurlSecretFiles secret_files;
    try {
        secret_files = WriteCurlSecretFiles(workspace_path, token_request.body, /*header_lines=*/{});
    } catch (const std::exception& error) {
        return {
            .success = false,
            .error = "OAuthRefreshTokenExchangeFailed",
            .error_description = std::string("could not stage OAuth refresh request body: ") + error.what(),
        };
    }
    const auto body_arg = std::string("@") + secret_files.body_file.string();

    const CliSpec spec{
        .name = "oauth_refresh_token_exchange",
        .description = "Refresh OAuth tokens through curl.",
        .binary = "curl",
        .args_template = {
            "-sS",
            "-X",
            "POST",
            "-H",
            "Content-Type: {{content_type}}",
            "--data",
            "{{token_request_body_file}}",
            "{{oauth_token_endpoint}}",
        },
        .required_args = {"oauth_token_endpoint", "content_type", "token_request_body_file"},
        .input_schema_json = R"({"type":"object"})",
        .output_schema_json = R"({"type":"object"})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = timeout_ms,
        .output_limit_bytes = 64 * 1024,
        .env_allowlist = {"PATH"},
    };

    const auto result = cli_host.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"oauth_token_endpoint", token_request.token_endpoint},
            {"content_type", token_request.content_type},
            {"token_request_body_file", body_arg},
        },
        .workspace_path = workspace_path,
    });
    if (!result.success) {
        return {
            .success = false,
            .error = result.error_code.empty() ? "OAuthRefreshTokenExchangeFailed" : result.error_code,
            .error_description = result.error_message.empty() ? result.stderr_text : result.error_message,
        };
    }
    return ParseOAuthTokenResponse(result.stdout_text);
}

AuthSession PersistOAuthTokenSession(
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const OAuthSessionPersistInput& input) {
    RequireSuccessfulTokenResponse(input.token_response);
    const auto profile_name = input.profile_name.empty() ? "default" : input.profile_name;
    const auto provider_name = ToString(input.provider);
    auto session = AuthSession{
        .session_id = MakeAuthSessionId(input.provider, input.mode, profile_name),
        .provider = input.provider,
        .mode = input.mode,
        .profile_name = profile_name,
        .account_label = input.account_label.empty() ? provider_name + ":" + profile_name : input.account_label,
        .managed_by_agentos = true,
        .managed_by_external_cli = false,
        .refresh_supported = !input.token_response.refresh_token.empty(),
        .headless_compatible = true,
        .access_token_ref = token_store.write_managed_token(
            provider_name,
            profile_name,
            "access_token",
            input.token_response.access_token),
        .expires_at = ExpiryFromTokenResponse(input.token_response),
        .metadata = {
            {"credential_source", "oauth_pkce"},
            {"token_type", input.token_response.token_type.empty() ? "Bearer" : input.token_response.token_type},
        },
    };
    if (!input.token_endpoint.empty()) {
        session.metadata["token_endpoint"] = input.token_endpoint;
    }
    if (!input.client_id.empty()) {
        session.metadata["client_id"] = input.client_id;
    }
    if (!input.token_response.refresh_token.empty()) {
        session.refresh_token_ref = token_store.write_managed_token(
            provider_name,
            profile_name,
            "refresh_token",
            input.token_response.refresh_token);
    }
    session_store.save(session);
    return session;
}

AuthSession PersistOAuthRefreshSession(
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const AuthSession& existing_session,
    const OAuthTokenResponse& token_response) {
    RequireSuccessfulTokenResponse(token_response);
    auto refreshed = existing_session;
    const auto provider_name = ToString(existing_session.provider);
    refreshed.managed_by_agentos = true;
    refreshed.managed_by_external_cli = false;
    refreshed.access_token_ref = token_store.write_managed_token(
        provider_name,
        existing_session.profile_name,
        "access_token",
        token_response.access_token);
    if (!token_response.refresh_token.empty()) {
        refreshed.refresh_token_ref = token_store.write_managed_token(
            provider_name,
            existing_session.profile_name,
            "refresh_token",
            token_response.refresh_token);
    }
    refreshed.refresh_supported = !refreshed.refresh_token_ref.empty();
    refreshed.expires_at = ExpiryFromTokenResponse(token_response);
    refreshed.metadata["refreshed_by"] = "oauth_refresh_token";
    refreshed.metadata["token_type"] = token_response.token_type.empty() ? "Bearer" : token_response.token_type;
    session_store.save(refreshed);
    return refreshed;
}

AuthSession CompleteOAuthLogin(
    const CliHost& cli_host,
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const OAuthLoginOrchestrationInput& input,
    const std::filesystem::path& workspace_path,
    const int timeout_ms) {
    if (!input.callback.success) {
        throw std::runtime_error(input.callback.error.empty() ? "OAuthCallbackFailed" : input.callback.error);
    }
    const auto token_response = ExecuteOAuthTokenExchange(
        cli_host,
        OAuthTokenRequestInput{
            .token_endpoint = input.token_endpoint,
            .client_id = input.client_id,
            .redirect_uri = input.start.redirect_uri,
            .code = input.callback.code,
            .code_verifier = input.start.code_verifier,
        },
        workspace_path,
        timeout_ms);
    return PersistOAuthTokenSession(
        session_store,
        token_store,
        OAuthSessionPersistInput{
            .provider = input.start.provider,
            .mode = AuthMode::browser_oauth,
            .profile_name = input.start.profile_name,
            .account_label = input.account_label,
            .token_endpoint = input.token_endpoint,
            .client_id = input.client_id,
            .token_response = token_response,
        });
}

AuthSession RefreshOAuthSession(
    const CliHost& cli_host,
    SessionStore& session_store,
    const SecureTokenStore& token_store,
    const OAuthRefreshOrchestrationInput& input,
    const std::filesystem::path& workspace_path,
    const int timeout_ms) {
    if (input.existing_session.refresh_token_ref.empty()) {
        throw std::runtime_error("RefreshTokenUnavailable");
    }
    const auto refresh_token = token_store.read_ref(input.existing_session.refresh_token_ref);
    if (!refresh_token.has_value() || refresh_token->empty()) {
        throw std::runtime_error("RefreshTokenUnavailable");
    }
    const auto token_response = ExecuteOAuthRefreshTokenExchange(
        cli_host,
        OAuthRefreshTokenRequestInput{
            .token_endpoint = input.token_endpoint,
            .client_id = input.client_id,
            .refresh_token = *refresh_token,
        },
        workspace_path,
        timeout_ms);
    return PersistOAuthRefreshSession(session_store, token_store, input.existing_session, token_response);
}

}  // namespace agentos
