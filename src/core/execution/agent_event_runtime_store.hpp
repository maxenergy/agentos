#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace agentos {

struct AgentEventStatusSummary {
    std::string state;
    int elapsed_ms = -1;
    int heartbeat_count = -1;
    std::string wait_policy;
    std::filesystem::path status_file;
};

class AgentEventRuntimeStore {
public:
    explicit AgentEventRuntimeStore(std::filesystem::path workspace);

    [[nodiscard]] std::optional<std::filesystem::path> find_task_dir(const std::string& task_id) const;
    [[nodiscard]] std::optional<AgentEventStatusSummary> read_latest_status(
        const std::filesystem::path& task_dir) const;

private:
    std::filesystem::path workspace_;
};

std::string FormatAgentEventStatusSummary(const AgentEventStatusSummary& summary);

}  // namespace agentos
