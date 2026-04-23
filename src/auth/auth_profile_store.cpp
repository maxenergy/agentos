#include "auth/auth_profile_store.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

std::string EncodeField(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        if (ch == '%' || ch == '\t' || ch == '\n' || ch == '\r') {
            constexpr char hex[] = "0123456789ABCDEF";
            output << '%' << hex[(ch >> 4) & 0x0F] << hex[ch & 0x0F];
        } else {
            output << static_cast<char>(ch);
        }
    }
    return output.str();
}

int HexValue(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

std::string DecodeField(const std::string& value) {
    std::string output;
    output.reserve(value.size());

    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = HexValue(value[index + 1]);
            const auto low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(value[index]);
    }

    return output;
}

std::vector<std::string> SplitLine(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;

    while (start <= line.size()) {
        const auto delimiter = line.find(kDelimiter, start);
        if (delimiter == std::string::npos) {
            parts.push_back(DecodeField(line.substr(start)));
            break;
        }

        parts.push_back(DecodeField(line.substr(start, delimiter - start)));
        start = delimiter + 1;
    }

    return parts;
}

}  // namespace

AuthProfileStore::AuthProfileStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

void AuthProfileStore::set_default(const AuthProviderId provider, const std::string& profile_name) {
    mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(), [&](const AuthProfileMapping& mapping) {
        return mapping.provider == provider;
    }), mappings_.end());
    mappings_.push_back(AuthProfileMapping{
        .provider = provider,
        .profile_name = profile_name.empty() ? "default" : profile_name,
    });
    flush();
}

std::optional<std::string> AuthProfileStore::default_profile(const AuthProviderId provider) const {
    const auto it = std::find_if(mappings_.begin(), mappings_.end(), [&](const AuthProfileMapping& mapping) {
        return mapping.provider == provider;
    });
    if (it == mappings_.end()) {
        return std::nullopt;
    }
    return it->profile_name;
}

std::vector<AuthProfileMapping> AuthProfileStore::list() const {
    return mappings_;
}

const std::filesystem::path& AuthProfileStore::store_path() const {
    return store_path_;
}

void AuthProfileStore::load() {
    mappings_.clear();
    std::ifstream input(store_path_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 2) {
            continue;
        }
        const auto provider = ParseAuthProviderId(parts[0]);
        if (!provider.has_value()) {
            continue;
        }
        mappings_.push_back(AuthProfileMapping{
            .provider = *provider,
            .profile_name = parts[1].empty() ? "default" : parts[1],
        });
    }
}

void AuthProfileStore::flush() const {
    if (!store_path_.parent_path().empty()) {
        std::filesystem::create_directories(store_path_.parent_path());
    }

    std::ofstream output(store_path_, std::ios::binary | std::ios::trunc);
    for (const auto& mapping : mappings_) {
        output
            << EncodeField(ToString(mapping.provider)) << kDelimiter
            << EncodeField(mapping.profile_name)
            << '\n';
    }
}

}  // namespace agentos
