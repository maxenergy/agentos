#pragma once

#include "core/models.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace agentos {

struct NormalizedAgentResultInput {
    std::string agent_name;
    bool success = false;
    std::string summary;
    std::string structured_output_json;
    std::vector<AgentArtifact> artifacts;
    int duration_ms = 0;
    double estimated_cost = 0.0;
    std::string error_code;
    std::string error_message;
};

std::string BuildNormalizedAgentResultJson(const NormalizedAgentResultInput& input);

}  // namespace agentos
