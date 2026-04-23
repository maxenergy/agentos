#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agentos {

struct WorkflowDefinition {
    std::string name;
    std::string trigger_task_type;
    std::vector<std::string> ordered_steps;
    std::string source = "manual";
    bool enabled = true;
    int use_count = 0;
    int success_count = 0;
    int failure_count = 0;
    double success_rate = 0.0;
    double avg_duration_ms = 0.0;
    double score = 0.0;
};

class WorkflowStore {
public:
    WorkflowStore() = default;
    explicit WorkflowStore(std::filesystem::path store_path);

    WorkflowDefinition save(WorkflowDefinition workflow);
    bool remove(const std::string& name);
    [[nodiscard]] std::optional<WorkflowDefinition> find(const std::string& name) const;
    [[nodiscard]] std::vector<WorkflowDefinition> list() const;
    [[nodiscard]] const std::filesystem::path& store_path() const;

    [[nodiscard]] static WorkflowDefinition FromCandidate(const WorkflowCandidate& candidate);

private:
    void load();
    void flush() const;

    std::filesystem::path store_path_;
    std::vector<WorkflowDefinition> workflows_;
};

}  // namespace agentos
