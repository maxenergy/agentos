#include "trust/pairing_invite_store.hpp"

#include "core/policy/permission_model.hpp"
#include "utils/atomic_file.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
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

std::string JoinList(const std::vector<std::string>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << values[index];
    }
    return output.str();
}

std::vector<std::string> SplitList(const std::string& value) {
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

long long ParseLongLong(const std::string& value, const long long fallback) {
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

bool ParseBool(const std::string& value) {
    return value == "true" || value == "1";
}

std::string MakeToken(const std::string& identity_id, const std::string& device_id, const long long created_epoch_ms) {
    const auto seed = identity_id + ":" + device_id + ":" + std::to_string(created_epoch_ms);
    std::ostringstream output;
    output << "pair-" << created_epoch_ms << '-'
           << std::hex << std::setw(16) << std::setfill('0') << std::hash<std::string>{}(seed);
    return output.str();
}

}  // namespace

PairingInviteStore::PairingInviteStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

PairingInvite PairingInviteStore::create(
    const std::string& identity_id,
    const std::string& device_id,
    const std::string& label,
    const std::string& user_id,
    const std::string& identity_label,
    std::vector<std::string> permissions,
    const int ttl_seconds) {
    if (identity_id.empty()) {
        throw std::invalid_argument("identity is required");
    }
    if (device_id.empty()) {
        throw std::invalid_argument("device is required");
    }
    if (ttl_seconds <= 0) {
        throw std::invalid_argument("ttl_seconds must be positive");
    }
    if (const auto unknown = PermissionModel::unknown_permissions(permissions); !unknown.empty()) {
        throw std::invalid_argument("unknown invite permissions: " + JoinList(unknown));
    }

    const auto now = NowEpochMs();
    PairingInvite invite{
        .token = MakeToken(identity_id, device_id, now),
        .identity_id = identity_id,
        .device_id = device_id,
        .label = label.empty() ? identity_id + ":" + device_id : label,
        .user_id = user_id.empty() ? "remote-user" : user_id,
        .identity_label = identity_label.empty() ? identity_id : identity_label,
        .permissions = permissions.empty() ? std::vector<std::string>{"task.submit"} : std::move(permissions),
        .created_epoch_ms = now,
        .expires_epoch_ms = now + (static_cast<long long>(ttl_seconds) * 1000LL),
        .consumed = false,
    };

    invites_.push_back(invite);
    flush();
    return invite;
}

std::optional<PairingInvite> PairingInviteStore::consume(const std::string& token) {
    const auto now = NowEpochMs();
    for (auto& invite : invites_) {
        if (invite.token != token || invite.consumed || invite.expires_epoch_ms <= now) {
            continue;
        }

        invite.consumed = true;
        const auto consumed = invite;
        flush();
        return consumed;
    }
    return std::nullopt;
}

std::vector<PairingInvite> PairingInviteStore::list_active() const {
    const auto now = NowEpochMs();
    std::vector<PairingInvite> active;
    for (const auto& invite : invites_) {
        if (!invite.consumed && invite.expires_epoch_ms > now) {
            active.push_back(invite);
        }
    }
    return active;
}

const std::filesystem::path& PairingInviteStore::store_path() const {
    return store_path_;
}

long long PairingInviteStore::NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void PairingInviteStore::load() {
    invites_.clear();

    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 10) {
            continue;
        }
        invites_.push_back(PairingInvite{
            .token = parts[0],
            .identity_id = parts[1],
            .device_id = parts[2],
            .label = parts[3],
            .user_id = parts[4],
            .identity_label = parts[5],
            .permissions = SplitList(parts[6]),
            .created_epoch_ms = ParseLongLong(parts[7], 0),
            .expires_epoch_ms = ParseLongLong(parts[8], 0),
            .consumed = ParseBool(parts[9]),
        });
    }
}

void PairingInviteStore::flush() const {
    std::ostringstream output;
    for (const auto& invite : invites_) {
        output
            << EncodeField(invite.token) << kDelimiter
            << EncodeField(invite.identity_id) << kDelimiter
            << EncodeField(invite.device_id) << kDelimiter
            << EncodeField(invite.label) << kDelimiter
            << EncodeField(invite.user_id) << kDelimiter
            << EncodeField(invite.identity_label) << kDelimiter
            << EncodeField(JoinList(invite.permissions)) << kDelimiter
            << invite.created_epoch_ms << kDelimiter
            << invite.expires_epoch_ms << kDelimiter
            << (invite.consumed ? "true" : "false")
            << '\n';
    }

    WriteFileAtomically(store_path_, output.str());
}

}  // namespace agentos
