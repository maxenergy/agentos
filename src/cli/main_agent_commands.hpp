#pragma once

#include "storage/main_agent_store.hpp"

namespace agentos {

// `agentos main-agent set | show | unset | list-providers`.
// Returns process exit code.
int RunMainAgentCommand(const MainAgentStore& store, int argc, char* argv[]);

}  // namespace agentos
