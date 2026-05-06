#include "autodev/autodev_execution_adapter.hpp"

#include <nlohmann/json.hpp>

namespace agentos {

[[nodiscard]] nlohmann::json ToJson(const AutoDevExecutionAdapterProfile& profile) {
    return nlohmann::json{
        {"adapter_kind", profile.adapter_kind},
        {"adapter_name", profile.adapter_name},
        {"supports_persistent_session", profile.supports_persistent_session},
        {"supports_native_event_stream", profile.supports_native_event_stream},
        {"supports_interrupt", profile.supports_interrupt},
        {"supports_realtime_diff", profile.supports_realtime_diff},
        {"supports_same_thread_repair", profile.supports_same_thread_repair},
        {"continuity_mode", profile.continuity_mode},
        {"event_stream_mode", profile.event_stream_mode},
        {"risk_level", profile.risk_level},
        {"production_final_executor", profile.production_final_executor},
    };
}

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

AutoDevExecutionAdapterProfile CodexAppServerAutoDevAdapterProfile() {
    return AutoDevExecutionAdapterProfile{
        .adapter_kind = "codex_app_server",
        .adapter_name = "CodexAppServerAutoDevAdapter",
        .supports_persistent_session = true,
        .supports_native_event_stream = true,
        .supports_interrupt = true,
        .supports_realtime_diff = true,
        .supports_same_thread_repair = true,
        .continuity_mode = "persistent_thread",
        .event_stream_mode = "native_app_server",
        .risk_level = "medium",
        .production_final_executor = false,
    };
}

AutoDevExecutionAdapterProfile CodexCliAutoDevAdapter::profile() const {
    return CodexCliAutoDevAdapterProfile();
}

bool CodexCliAutoDevAdapter::healthy() const {
    return false;
}

AutoDevExecutionAdapterProfile CodexAppServerAutoDevAdapter::profile() const {
    return CodexAppServerAutoDevAdapterProfile();
}

bool CodexAppServerAutoDevAdapter::healthy() const {
    return false;
}

}  // namespace agentos
