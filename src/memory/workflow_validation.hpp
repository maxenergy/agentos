#pragma once

#include "memory/workflow_store.hpp"

#include <string>
#include <vector>

namespace agentos {

struct WorkflowValidationDiagnostic {
    std::string workflow_name;
    std::string field;
    std::string value;
    std::string reason;
};

struct WorkflowApplicabilityCheck {
    std::string field;
    std::string value;
    bool matched = false;
    std::string reason;
};

struct WorkflowApplicabilityEvaluation {
    std::string workflow_name;
    bool applicable = false;
    std::vector<WorkflowApplicabilityCheck> checks;
};

[[nodiscard]] std::vector<WorkflowValidationDiagnostic> ValidateWorkflowDefinition(
    const WorkflowDefinition& workflow);

[[nodiscard]] WorkflowApplicabilityEvaluation EvaluateWorkflowApplicability(
    const WorkflowDefinition& workflow,
    const std::string& task_type,
    const StringMap& inputs);

}  // namespace agentos
