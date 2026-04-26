#include "trust/allowlist_store.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <sstream>
#include <utility>

namespace agentos {

namespace {

constexpr char kDelimiter = '\t';

std::string JoinPermissions(const std::vector<std::string>& permissions) {
    std::ostringstream output;
    for (std::size_t index = 0; index < permissions.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << permissions[index];
    }
    return output.str();
}

std::vector<std::string> SplitPermissions(const std::string& value) {
    std::vector<std::string> permissions;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) {
            permissions.push_back(std::move(item));
        }
    }
    return permissions;
}

std::vector<std::string> SplitLine(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream input(line);
    std::string item;
    while (std::getline(input, item, kDelimiter)) {
        parts.push_back(std::move(item));
    }
    return parts;
}

long long ParseLongLong(const std::string& value, const long long fallback) {
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

AllowlistStore::AllowlistStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

void AllowlistStore::save(const TrustedPeer& peer) {
    remove(peer.identity_id, peer.device_id);
    peers_.push_back(peer);
    flush();
}

std::optional<TrustedPeer> AllowlistStore::find(const std::string& identity_id, const std::string& device_id) const {
    const auto it = std::find_if(peers_.begin(), peers_.end(), [&](const TrustedPeer& peer) {
        return peer.identity_id == identity_id && peer.device_id == device_id;
    });

    if (it == peers_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::vector<TrustedPeer> AllowlistStore::list() const {
    return peers_;
}

void AllowlistStore::remove(const std::string& identity_id, const std::string& device_id) {
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const TrustedPeer& peer) {
        return peer.identity_id == identity_id && peer.device_id == device_id;
    }), peers_.end());
    flush();
}

const std::filesystem::path& AllowlistStore::store_path() const {
    return store_path_;
}

void AllowlistStore::load() {
    peers_.clear();

    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 5) {
            continue;
        }

        peers_.push_back(TrustedPeer{
            .identity_id = parts[0],
            .device_id = parts[1],
            .label = parts[2],
            .trust_level = ParseTrustLevel(parts[3]),
            .permissions = SplitPermissions(parts[4]),
            .paired_epoch_ms = parts.size() > 5 ? ParseLongLong(parts[5], 0) : 0,
            .last_seen_epoch_ms = parts.size() > 6 ? ParseLongLong(parts[6], 0) : 0,
        });
    }
}

void AllowlistStore::flush() const {
    std::ostringstream output;
    for (const auto& peer : peers_) {
        output
            << peer.identity_id << kDelimiter
            << peer.device_id << kDelimiter
            << peer.label << kDelimiter
            << ToString(peer.trust_level) << kDelimiter
            << JoinPermissions(peer.permissions) << kDelimiter
            << peer.paired_epoch_ms << kDelimiter
            << peer.last_seen_epoch_ms
            << '\n';
    }

    WriteFileAtomically(store_path_, output.str());
}

}  // namespace agentos
