#pragma once

#include "core/models.hpp"
#include "core/registry/agent_registry.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

struct WorkspaceAgentSession {
    std::string workspace_session_id;
    std::filesystem::path workspace_path;
    std::string agent_name;
    std::string agent_session_id;
    bool active = false;
};

class WorkspaceSession {
public:
    WorkspaceSession(
        AgentRegistry& agent_registry,
        std::filesystem::path workspace_path,
        std::string workspace_session_id);
    WorkspaceSession(const WorkspaceSession&) = delete;
    WorkspaceSession& operator=(const WorkspaceSession&) = delete;
    ~WorkspaceSession();

    [[nodiscard]] const std::string& session_id() const;
    [[nodiscard]] const std::filesystem::path& workspace_path() const;
    [[nodiscard]] std::vector<WorkspaceAgentSession> sessions() const;
    [[nodiscard]] std::optional<WorkspaceAgentSession> find(const std::string& agent_name) const;

    bool open_agent(const std::string& agent_name, const std::string& session_config_json = "{}");
    AgentResult run_agent_task(const std::string& agent_name, AgentTask task);
    bool close_agent(const std::string& agent_name);
    void close_all();

private:
    AgentRegistry& agent_registry_;
    std::filesystem::path workspace_path_;
    std::string workspace_session_id_;
    std::unordered_map<std::string, WorkspaceAgentSession> sessions_by_agent_;
};

}  // namespace agentos
