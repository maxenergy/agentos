#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

struct ApprovalRecord {
    std::string approval_id;
    std::string subject;
    std::string reason;
    std::string requested_by;
    std::string approved_by;
    std::string status;
    long long created_epoch_ms = 0;
    long long decided_epoch_ms = 0;
};

class ApprovalStore {
public:
    explicit ApprovalStore(std::filesystem::path store_path);

    ApprovalRecord request(std::string subject, std::string reason, std::string requested_by);
    bool approve(const std::string& approval_id, const std::string& approved_by);
    bool revoke(const std::string& approval_id, const std::string& approved_by);
    std::optional<ApprovalRecord> find(const std::string& approval_id) const;
    std::vector<ApprovalRecord> list() const;
    bool is_approved(const std::string& approval_id) const;

    [[nodiscard]] const std::filesystem::path& store_path() const;

    static long long NowEpochMs();

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<ApprovalRecord> approvals_;
};

}  // namespace agentos
