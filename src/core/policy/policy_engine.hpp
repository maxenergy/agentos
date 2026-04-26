#pragma once

#include "core/models.hpp"
#include "core/policy/approval_store.hpp"
#include "core/policy/role_catalog.hpp"
#include "trust/trust_policy.hpp"

namespace agentos {

struct PolicyEngineDependencies {
    const TrustPolicy* trust_policy = nullptr;
    const RoleCatalog* role_catalog = nullptr;
    const ApprovalStore* approval_store = nullptr;
};

class PolicyEngine {
public:
    explicit PolicyEngine(PolicyEngineDependencies dependencies = {});

    PolicyDecision evaluate_skill(const TaskRequest& task, const SkillManifest& manifest, const SkillCall& call) const;
    PolicyDecision evaluate_agent(const TaskRequest& task, const AgentProfile& profile, const AgentTask& agent_task) const;

private:
    PolicyDecision evaluate_task_origin(const TaskRequest& task) const;
    std::vector<std::string> effective_permission_grants(const TaskRequest& task) const;

    PolicyEngineDependencies dependencies_;
};

}  // namespace agentos
