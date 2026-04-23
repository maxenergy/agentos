#pragma once

#include "core/models.hpp"
#include "trust/trust_policy.hpp"

namespace agentos {

class PolicyEngine {
public:
    PolicyEngine() = default;
    explicit PolicyEngine(const TrustPolicy& trust_policy);

    PolicyDecision evaluate_skill(const TaskRequest& task, const SkillManifest& manifest, const SkillCall& call) const;
    PolicyDecision evaluate_agent(const TaskRequest& task, const AgentProfile& profile, const AgentTask& agent_task) const;

private:
    PolicyDecision evaluate_task_origin(const TaskRequest& task) const;

    const TrustPolicy* trust_policy_ = nullptr;
};

}  // namespace agentos
