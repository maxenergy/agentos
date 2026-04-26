#include "core/policy/approval_store.hpp"

#include "utils/atomic_file.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
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

long long ParseLongLong(const std::string& value, const long long fallback) {
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string MakeApprovalId(const std::string& subject, const long long created_epoch_ms) {
    std::ostringstream output;
    output << "approval-" << created_epoch_ms << '-'
           << std::hex << std::setw(16) << std::setfill('0')
           << std::hash<std::string>{}(subject + ":" + std::to_string(created_epoch_ms));
    return output.str();
}

}  // namespace

ApprovalStore::ApprovalStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {
    load();
}

ApprovalRecord ApprovalStore::request(std::string subject, std::string reason, std::string requested_by) {
    if (subject.empty()) {
        throw std::invalid_argument("approval subject is required");
    }

    const auto now = NowEpochMs();
    ApprovalRecord record{
        .approval_id = MakeApprovalId(subject, now),
        .subject = std::move(subject),
        .reason = std::move(reason),
        .requested_by = requested_by.empty() ? "local-user" : std::move(requested_by),
        .approved_by = "",
        .status = "pending",
        .created_epoch_ms = now,
        .decided_epoch_ms = 0,
    };
    approvals_.push_back(record);
    flush();
    return record;
}

bool ApprovalStore::approve(const std::string& approval_id, const std::string& approved_by) {
    for (auto& record : approvals_) {
        if (record.approval_id != approval_id) {
            continue;
        }
        record.status = "approved";
        record.approved_by = approved_by.empty() ? "local-admin" : approved_by;
        record.decided_epoch_ms = NowEpochMs();
        flush();
        return true;
    }
    return false;
}

bool ApprovalStore::revoke(const std::string& approval_id, const std::string& approved_by) {
    for (auto& record : approvals_) {
        if (record.approval_id != approval_id) {
            continue;
        }
        record.status = "revoked";
        record.approved_by = approved_by.empty() ? "local-admin" : approved_by;
        record.decided_epoch_ms = NowEpochMs();
        flush();
        return true;
    }
    return false;
}

std::optional<ApprovalRecord> ApprovalStore::find(const std::string& approval_id) const {
    const auto found = std::find_if(approvals_.begin(), approvals_.end(), [&](const ApprovalRecord& record) {
        return record.approval_id == approval_id;
    });
    if (found == approvals_.end()) {
        return std::nullopt;
    }
    return *found;
}

std::vector<ApprovalRecord> ApprovalStore::list() const {
    return approvals_;
}

bool ApprovalStore::is_approved(const std::string& approval_id) const {
    const auto record = find(approval_id);
    return record.has_value() && record->status == "approved";
}

const std::filesystem::path& ApprovalStore::store_path() const {
    return store_path_;
}

long long ApprovalStore::NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void ApprovalStore::load() {
    approvals_.clear();
    std::ifstream input(store_path_, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const auto parts = SplitLine(line);
        if (parts.size() < 8) {
            continue;
        }
        approvals_.push_back(ApprovalRecord{
            .approval_id = parts[0],
            .subject = parts[1],
            .reason = parts[2],
            .requested_by = parts[3],
            .approved_by = parts[4],
            .status = parts[5],
            .created_epoch_ms = ParseLongLong(parts[6], 0),
            .decided_epoch_ms = ParseLongLong(parts[7], 0),
        });
    }
}

void ApprovalStore::flush() const {
    std::ostringstream output;
    for (const auto& record : approvals_) {
        output
            << EncodeField(record.approval_id) << kDelimiter
            << EncodeField(record.subject) << kDelimiter
            << EncodeField(record.reason) << kDelimiter
            << EncodeField(record.requested_by) << kDelimiter
            << EncodeField(record.approved_by) << kDelimiter
            << EncodeField(record.status) << kDelimiter
            << record.created_epoch_ms << kDelimiter
            << record.decided_epoch_ms
            << '\n';
    }
    WriteFileAtomically(store_path_, output.str());
}

}  // namespace agentos
