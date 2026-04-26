#pragma once

#include <string>

namespace agentos {

struct StoragePolicyDecision {
    std::string decision_id;
    std::string backend;
    std::string target_backend;
    std::string migration_status;
    std::string rationale;
    std::string revisit_trigger;
    std::string migration_boundary;
    std::string required_interface;
    std::string compatibility_contract;
};

StoragePolicyDecision CurrentStoragePolicy();

}  // namespace agentos
