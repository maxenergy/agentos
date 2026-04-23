#pragma once

#include "core/models.hpp"
#include "core/registry/skill_registry.hpp"

namespace agentos {

class WorkflowRunSkill final : public ISkillAdapter {
public:
    explicit WorkflowRunSkill(const SkillRegistry& skill_registry);

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

private:
    SkillResult RunWritePatchRead(const SkillCall& call) const;

    const SkillRegistry& skill_registry_;
};

}  // namespace agentos

