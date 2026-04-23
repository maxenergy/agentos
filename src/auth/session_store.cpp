#include "auth/session_store.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

long long ToEpochSeconds(const std::chrono::system_clock::time_point value) {
    if (value == std::chrono::system_clock::time_point::max()) {
        return -1;
    }

    return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point FromEpochSeconds(const long long value) {
    if (value < 0) {
        return std::chrono::system_clock::time_point::max();
    }

    return std::chrono::system_clock::time_point(std::chrono::seconds(value));
}

std::vector<std::string> SplitLine(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string part;

    while (std::getline(stream, part, kDelimiter)) {
        parts.push_back(std::move(part));
    }

    return parts;
}

std::string MetadataToString(const std::map<std::string, std::string>& metadata) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) {
            stream << ';';
        }
        first = false;
        stream << key << '=' << value;
    }
    return stream.str();
}

std::map<std::string, std::string> MetadataFromString(const std::string& value) {
    std::map<std::string, std::string> metadata;
    std::stringstream stream(value);
    std::string item;

    while (std::getline(stream, item, ';')) {
        const auto separator = item.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        metadata[item.substr(0, separator)] = item.substr(separator + 1);
    }

    return metadata;
}

}  // namespace

SessionStore::SessionStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

void SessionStore::save(const AuthSession& session) {
    remove(session.provider, session.profile_name);
    sessions_.push_back(session);
    flush();
}

std::optional<AuthSession> SessionStore::find(const AuthProviderId provider, const std::string& profile_name) const {
    const auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const AuthSession& session) {
        return session.provider == provider && session.profile_name == profile_name;
    });

    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<AuthSession> SessionStore::list() const {
    return sessions_;
}

void SessionStore::remove(const AuthProviderId provider, const std::string& profile_name) {
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const AuthSession& session) {
        return session.provider == provider && session.profile_name == profile_name;
    }), sessions_.end());
    flush();
}

const std::filesystem::path& SessionStore::store_path() const {
    return store_path_;
}

void SessionStore::load() {
    sessions_.clear();

    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 13) {
            continue;
        }

        const auto provider = ParseAuthProviderId(parts[1]);
        const auto mode = ParseAuthMode(parts[2]);
        if (!provider.has_value() || !mode.has_value()) {
            continue;
        }

        AuthSession session;
        session.session_id = parts[0];
        session.provider = *provider;
        session.mode = *mode;
        session.profile_name = parts[3];
        session.account_label = parts[4];
        session.managed_by_agentos = parts[5] == "1";
        session.managed_by_external_cli = parts[6] == "1";
        session.refresh_supported = parts[7] == "1";
        session.headless_compatible = parts[8] == "1";
        session.access_token_ref = parts[9];
        session.refresh_token_ref = parts[10];
        session.expires_at = FromEpochSeconds(std::stoll(parts[11]));
        session.metadata = MetadataFromString(parts[12]);
        sessions_.push_back(std::move(session));
    }
}

void SessionStore::flush() const {
    if (!store_path_.parent_path().empty()) {
        std::filesystem::create_directories(store_path_.parent_path());
    }

    std::ofstream output(store_path_, std::ios::binary | std::ios::trunc);
    for (const auto& session : sessions_) {
        output
            << session.session_id << kDelimiter
            << ToString(session.provider) << kDelimiter
            << ToString(session.mode) << kDelimiter
            << session.profile_name << kDelimiter
            << session.account_label << kDelimiter
            << (session.managed_by_agentos ? "1" : "0") << kDelimiter
            << (session.managed_by_external_cli ? "1" : "0") << kDelimiter
            << (session.refresh_supported ? "1" : "0") << kDelimiter
            << (session.headless_compatible ? "1" : "0") << kDelimiter
            << session.access_token_ref << kDelimiter
            << session.refresh_token_ref << kDelimiter
            << ToEpochSeconds(session.expires_at) << kDelimiter
            << MetadataToString(session.metadata)
            << '\n';
    }
}

}  // namespace agentos

