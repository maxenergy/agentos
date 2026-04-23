#include "core/orchestration/workspace_session.hpp"

#include "utils/path_utils.hpp"

#include <algorithm>

namespace agentos {

WorkspaceSession::WorkspaceSession(
    AgentRegistry& agent_registry,
    std::filesystem::path workspace_path,
    std::string workspace_session_id)
    : agent_registry_(agent_registry),
      workspace_path_(NormalizeWorkspaceRoot(std::move(workspace_path))),
      workspace_session_id_(std::move(workspace_session_id)) {}

WorkspaceSession::~WorkspaceSession() {
    close_all();
}

const std::string& WorkspaceSession::session_id() const {
    return workspace_session_id_;
}

const std::filesystem::path& WorkspaceSession::workspace_path() const {
    return workspace_path_;
}

std::vector<WorkspaceAgentSession> WorkspaceSession::sessions() const {
    std::vector<WorkspaceAgentSession> sessions;
    sessions.reserve(sessions_by_agent_.size());

    for (const auto& [unused_name, session] : sessions_by_agent_) {
        (void)unused_name;
        sessions.push_back(session);
    }

    std::sort(sessions.begin(), sessions.end(), [](const WorkspaceAgentSession& left, const WorkspaceAgentSession& right) {
        return left.agent_name < right.agent_name;
    });
    return sessions;
}

std::optional<WorkspaceAgentSession> WorkspaceSession::find(const std::string& agent_name) const {
    const auto it = sessions_by_agent_.find(agent_name);
    if (it == sessions_by_agent_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool WorkspaceSession::open_agent(const std::string& agent_name, const std::string& session_config_json) {
    if (const auto existing = find(agent_name); existing.has_value() && existing->active) {
        return true;
    }

    const auto agent = agent_registry_.find(agent_name);
    if (!agent || !agent->healthy() || !agent->profile().supports_session) {
        return false;
    }

    const auto agent_session_id = agent->start_session(session_config_json);
    if (agent_session_id.empty()) {
        return false;
    }

    sessions_by_agent_[agent_name] = WorkspaceAgentSession{
        .workspace_session_id = workspace_session_id_,
        .workspace_path = workspace_path_,
        .agent_name = agent_name,
        .agent_session_id = agent_session_id,
        .active = true,
    };
    return true;
}

AgentResult WorkspaceSession::run_agent_task(const std::string& agent_name, AgentTask task) {
    const auto session = find(agent_name);
    if (!session.has_value() || !session->active) {
        return {
            .success = false,
            .error_code = "WorkspaceSessionNotOpen",
            .error_message = "agent session is not open in this workspace session",
        };
    }

    const auto agent = agent_registry_.find(agent_name);
    if (!agent || !agent->healthy()) {
        return {
            .success = false,
            .error_code = "AgentUnavailable",
            .error_message = "agent was not found or is unhealthy",
        };
    }

    if (task.workspace_path.empty()) {
        task.workspace_path = workspace_path_.string();
    } else if (!IsPathInsideWorkspace(workspace_path_, task.workspace_path)) {
        return {
            .success = false,
            .error_code = "WorkspaceEscapeDenied",
            .error_message = "agent task workspace must stay inside the workspace session root",
        };
    }

    return agent->run_task_in_session(session->agent_session_id, task);
}

bool WorkspaceSession::close_agent(const std::string& agent_name) {
    const auto it = sessions_by_agent_.find(agent_name);
    if (it == sessions_by_agent_.end() || !it->second.active) {
        return false;
    }

    if (const auto agent = agent_registry_.find(agent_name); agent) {
        agent->close_session(it->second.agent_session_id);
    }
    it->second.active = false;
    return true;
}

void WorkspaceSession::close_all() {
    for (auto& [agent_name, session] : sessions_by_agent_) {
        if (!session.active) {
            continue;
        }
        if (const auto agent = agent_registry_.find(agent_name); agent) {
            agent->close_session(session.agent_session_id);
        }
        session.active = false;
    }
}

}  // namespace agentos
