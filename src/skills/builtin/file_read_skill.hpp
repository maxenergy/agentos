#pragma once

#include "core/models.hpp"

namespace agentos {

class FileReadSkill final : public ISkillAdapter {
public:
    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;
};

}  // namespace agentos

