#include "autodev/autodev_execution_adapter.hpp"

namespace agentos {

AutoDevExecutionAdapterProfile CodexCliAutoDevAdapterProfile() {
    return AutoDevExecutionAdapterProfile{
        .adapter_kind = "codex_cli",
        .adapter_name = "CodexCliAutoDevAdapter",
        .supports_persistent_session = false,
        .supports_native_event_stream = false,
        .supports_interrupt = false,
        .supports_realtime_diff = false,
        .supports_same_thread_repair = false,
        .continuity_mode = "best_effort_context",
        .event_stream_mode = "synthetic",
        .risk_level = "high",
        .production_final_executor = false,
    };
}

AutoDevExecutionAdapterProfile CodexCliAutoDevAdapter::profile() const {
    return CodexCliAutoDevAdapterProfile();
}

bool CodexCliAutoDevAdapter::healthy() const {
    return false;
}

}  // namespace agentos
