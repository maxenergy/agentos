#pragma once

#include "autodev/autodev_models.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace agentos {

struct AutoDevSubmitRequest {
    std::filesystem::path agentos_workspace;
    std::filesystem::path target_repo_path;
    std::string objective;
    std::optional<std::filesystem::path> skill_pack_path;
    std::string isolation_mode = "git_worktree";
};

struct AutoDevSubmitResult {
    bool success = false;
    std::string error_message;
    AutoDevJob job;
    std::filesystem::path job_dir;
};

class AutoDevStateStore {
public:
    explicit AutoDevStateStore(std::filesystem::path agentos_workspace);

    [[nodiscard]] const std::filesystem::path& agentos_workspace() const;
    [[nodiscard]] std::filesystem::path root_dir() const;
    [[nodiscard]] std::filesystem::path jobs_dir() const;
    [[nodiscard]] std::filesystem::path job_dir(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path job_json_path(const std::string& job_id) const;
    [[nodiscard]] std::filesystem::path events_path(const std::string& job_id) const;

    AutoDevSubmitResult submit(const AutoDevSubmitRequest& request);
    std::optional<AutoDevJob> load_job(const std::string& job_id, std::string* error_message = nullptr) const;

private:
    std::filesystem::path agentos_workspace_;
};

}  // namespace agentos
