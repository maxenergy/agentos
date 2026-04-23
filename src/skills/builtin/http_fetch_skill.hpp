#pragma once

#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"

namespace agentos {

class HttpFetchSkill final : public ISkillAdapter {
public:
    explicit HttpFetchSkill(const CliHost& cli_host);

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

private:
    const CliHost& cli_host_;
};

}  // namespace agentos

