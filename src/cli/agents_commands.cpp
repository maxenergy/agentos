#include "cli/agents_commands.hpp"

#include <iostream>

namespace agentos {

int RunAgentsCommand(const AgentRegistry& agent_registry) {
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto agent = agent_registry.find(profile.agent_name);
        std::cout
            << profile.agent_name
            << " healthy=" << (agent && agent->healthy() ? "true" : "false")
            << " session=" << (profile.supports_session ? "true" : "false")
            << " patch=" << (profile.supports_patch ? "true" : "false")
            << " risk=" << profile.risk_level
            << '\n';
    }
    return 0;
}

}  // namespace agentos
