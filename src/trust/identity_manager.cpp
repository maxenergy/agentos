#include "trust/identity_manager.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

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

IdentityManager::IdentityManager(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

Identity IdentityManager::save(Identity identity) {
    remove(identity.identity_id);
    identities_.push_back(std::move(identity));
    flush();
    return identities_.back();
}

Identity IdentityManager::ensure(const std::string& identity_id, const std::string& user_id, const std::string& label) {
    if (const auto existing = find(identity_id); existing.has_value()) {
        return *existing;
    }

    return save(Identity{
        .identity_id = identity_id,
        .user_id = user_id.empty() ? "remote-user" : user_id,
        .label = label.empty() ? identity_id : label,
    });
}

bool IdentityManager::remove(const std::string& identity_id) {
    const auto previous_size = identities_.size();
    identities_.erase(std::remove_if(identities_.begin(), identities_.end(), [&](const Identity& identity) {
        return identity.identity_id == identity_id;
    }), identities_.end());

    const bool removed = previous_size != identities_.size();
    if (removed) {
        flush();
    }
    return removed;
}

std::optional<Identity> IdentityManager::find(const std::string& identity_id) const {
    const auto it = std::find_if(identities_.begin(), identities_.end(), [&](const Identity& identity) {
        return identity.identity_id == identity_id;
    });

    if (it == identities_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<Identity> IdentityManager::list() const {
    return identities_;
}

const std::filesystem::path& IdentityManager::store_path() const {
    return store_path_;
}

void IdentityManager::load() {
    identities_.clear();

    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 3) {
            continue;
        }

        identities_.push_back(Identity{
            .identity_id = parts[0],
            .user_id = parts[1],
            .label = parts[2],
        });
    }
}

void IdentityManager::flush() const {
    if (!store_path_.parent_path().empty()) {
        std::filesystem::create_directories(store_path_.parent_path());
    }

    std::ofstream output(store_path_, std::ios::binary | std::ios::trunc);
    for (const auto& identity : identities_) {
        output
            << EncodeField(identity.identity_id) << kDelimiter
            << EncodeField(identity.user_id) << kDelimiter
            << EncodeField(identity.label)
            << '\n';
    }
}

}  // namespace agentos
