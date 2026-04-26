#pragma once

#include "core/loop/agent_loop.hpp"
#include "scheduler/scheduler.hpp"

#include <filesystem>

namespace agentos {

int RunScheduleCommand(
    Scheduler& scheduler,
    AgentLoop& loop,
    const std::filesystem::path& workspace,
    int argc,
    char* argv[]);

}  // namespace agentos
