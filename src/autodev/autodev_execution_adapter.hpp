#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

namespace agentos {

struct AutoDevExecutionAdapterProfile {
    std::string adapter_kind;
    std::string adapter_name;
    bool supports_persistent_session = false;
    bool supports_native_event_stream = false;
    bool supports_interrupt = false;
    bool supports_realtime_diff = false;
    bool supports_same_thread_repair = false;
    std::string continuity_mode;
    std::string event_stream_mode;
    std::string risk_level;
    bool production_final_executor = false;
};

class AutoDevExecutionAdapter {
public:
    virtual ~AutoDevExecutionAdapter() = default;

    [[nodiscard]] virtual AutoDevExecutionAdapterProfile profile() const = 0;
    [[nodiscard]] virtual bool healthy() const = 0;
};

class CodexCliAutoDevAdapter final : public AutoDevExecutionAdapter {
public:
    [[nodiscard]] AutoDevExecutionAdapterProfile profile() const override;
    [[nodiscard]] bool healthy() const override;
};

class CodexAppServerAutoDevAdapter final : public AutoDevExecutionAdapter {
public:
    [[nodiscard]] AutoDevExecutionAdapterProfile profile() const override;
    [[nodiscard]] bool healthy() const override;
};

[[nodiscard]] nlohmann::json ToJson(const AutoDevExecutionAdapterProfile& profile);
AutoDevExecutionAdapterProfile CodexCliAutoDevAdapterProfile();
AutoDevExecutionAdapterProfile CodexAppServerAutoDevAdapterProfile();

}  // namespace agentos
