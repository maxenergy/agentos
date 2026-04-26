#pragma once

#include "auth/auth_profile_store.hpp"
#include "auth/session_store.hpp"
#include "core/audit/audit_logger.hpp"
#include "core/execution/execution_cache.hpp"
#include "memory/memory_manager.hpp"
#include "scheduler/scheduler.hpp"
#include "storage/storage_version_store.hpp"

namespace agentos {

int RunStorageCommand(
    StorageVersionStore& storage_version_store,
    SessionStore& session_store,
    AuthProfileStore& auth_profile_store,
    ExecutionCache& execution_cache,
    MemoryManager& memory_manager,
    Scheduler& scheduler,
    AuditLogger& audit_logger,
    int argc,
    char* argv[]);

}  // namespace agentos
