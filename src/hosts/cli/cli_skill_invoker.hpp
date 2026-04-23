#pragma once

#include "core/models.hpp"
#include "hosts/cli/cli_host.hpp"

namespace agentos {

class CliSkillInvoker final : public ISkillAdapter {
public:
    CliSkillInvoker(CliSpec spec, const CliHost& cli_host);

    SkillManifest manifest() const override;
    SkillResult execute(const SkillCall& call) override;
    bool healthy() const override;

private:
    CliSpec spec_;
    const CliHost& cli_host_;
};

CliSpec MakeRgSearchSpec();
CliSpec MakeGitStatusSpec();
CliSpec MakeGitDiffSpec();
CliSpec MakeCurlFetchSpec();
CliSpec MakeJqTransformSpec();

}  // namespace agentos
