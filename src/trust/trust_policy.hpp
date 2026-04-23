#pragma once

#include "core/models.hpp"
#include "trust/allowlist_store.hpp"

namespace agentos {

class TrustPolicy {
public:
    explicit TrustPolicy(const AllowlistStore& allowlist_store);

    PolicyDecision evaluate_task_origin(const TaskRequest& task) const;

private:
    const AllowlistStore& allowlist_store_;
};

}  // namespace agentos

