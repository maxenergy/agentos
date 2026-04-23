#pragma once

#include "core/models.hpp"
#include "core/registry/skill_registry.hpp"
#include "memory/workflow_store.hpp"

namespace agentos {

class WorkflowRunSkill final : public ISkillAdapter {
public:
    explicit WorkflowRunSkill(const SkillRegistry& skill_registry, const WorkflowStore* workflow_store = nullptr);

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

private:
    SkillResult RunWritePatchRead(const SkillCall& call) const;
    SkillResult RunStoredWorkflow(const WorkflowDefinition& workflow, const SkillCall& call) const;
    bool StoredStepIsInPolicyScope(const SkillManifest& manifest) const;

    const SkillRegistry& skill_registry_;
    const WorkflowStore* workflow_store_ = nullptr;
};

}  // namespace agentos
