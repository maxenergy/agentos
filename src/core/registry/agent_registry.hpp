#pragma once

#include "core/models.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

class AgentRegistry {
public:
    bool register_agent(const std::shared_ptr<IAgentAdapter>& agent);
    std::shared_ptr<IAgentAdapter> find(const std::string& name) const;
    std::shared_ptr<IAgentAdapter> first_healthy() const;
    std::vector<AgentProfile> list_profiles() const;

private:
    std::unordered_map<std::string, std::shared_ptr<IAgentAdapter>> agents_;
    std::vector<std::string> registration_order_;
};

}  // namespace agentos
