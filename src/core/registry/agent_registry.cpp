#include "core/registry/agent_registry.hpp"

namespace agentos {

bool AgentRegistry::register_agent(const std::shared_ptr<IAgentAdapter>& agent) {
    if (!agent) {
        return false;
    }

    const auto name = agent->profile().agent_name;
    if (!agents_.contains(name)) {
        registration_order_.push_back(name);
    }
    agents_[name] = agent;
    return true;
}

std::shared_ptr<IAgentAdapter> AgentRegistry::find(const std::string& name) const {
    const auto it = agents_.find(name);
    if (it == agents_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<IAgentAdapter> AgentRegistry::first_healthy() const {
    for (const auto& name : registration_order_) {
        const auto agent = find(name);
        if (agent && agent->healthy()) {
            return agent;
        }
    }
    return nullptr;
}

std::vector<AgentProfile> AgentRegistry::list_profiles() const {
    std::vector<AgentProfile> profiles;
    profiles.reserve(agents_.size());

    for (const auto& name : registration_order_) {
        const auto agent = find(name);
        if (agent) {
            profiles.push_back(agent->profile());
        }
    }

    return profiles;
}

}  // namespace agentos
