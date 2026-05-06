#include "autodev/autodev_execution_adapter.hpp"

#include <optional>
#include <regex>
#include <stdexcept>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace agentos {

namespace {

struct ParsedHttpUrl {
    std::string scheme;
    std::string host;
    int port = 80;
};

std::optional<ParsedHttpUrl> ParseHttpUrl(const std::string& base_url) {
    static const std::regex pattern(R"(^(http)://([^/:]+)(?::([0-9]+))?/?$)");
    std::smatch match;
    if (!std::regex_match(base_url, match, pattern)) {
        return std::nullopt;
    }
    ParsedHttpUrl parsed;
    parsed.scheme = match[1].str();
    parsed.host = match[2].str();
    if (match[3].matched) {
        parsed.port = std::stoi(match[3].str());
    }
    return parsed;
}

httplib::Client HttpClientFor(const std::string& base_url) {
    const auto parsed = ParseHttpUrl(base_url);
    if (!parsed.has_value()) {
        throw std::runtime_error("codex app-server URL must be http://host[:port]");
    }
    httplib::Client client(parsed->host, parsed->port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(10, 0);
    return client;
}

}  // namespace

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
        .production_final_executor = true,
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
        .production_final_executor = true,
    };
}

AutoDevExecutionAdapterProfile CodexCliAutoDevAdapter::profile() const {
    return CodexCliAutoDevAdapterProfile();
}

bool CodexCliAutoDevAdapter::healthy() const {
    return true;
}

CodexAppServerAutoDevAdapter::CodexAppServerAutoDevAdapter(std::string base_url)
    : base_url_(std::move(base_url)) {}

AutoDevExecutionAdapterProfile CodexAppServerAutoDevAdapter::profile() const {
    return CodexAppServerAutoDevAdapterProfile();
}

bool CodexAppServerAutoDevAdapter::healthy() const {
    if (base_url_.empty()) {
        return false;
    }
    try {
        auto client = HttpClientFor(base_url_);
        const auto response = client.Get("/health");
        if (!response || response->status < 200 || response->status >= 300) {
            return false;
        }
        const auto body = nlohmann::json::parse(response->body);
        if (body.contains("healthy") && body.at("healthy").is_boolean()) {
            return body.at("healthy").get<bool>();
        }
        return body.value("status", "") == "ok";
    } catch (...) {
        return false;
    }
}

std::string CodexAppServerAutoDevAdapter::start_session(
    const std::string& job_id,
    const std::string& task_id) const {
    auto client = HttpClientFor(base_url_);
    const auto body = nlohmann::json{
        {"job_id", job_id},
        {"task_id", task_id},
        {"continuity_mode", "persistent_thread"},
    }.dump();
    const auto response = client.Post("/sessions", body, "application/json");
    if (!response || response->status < 200 || response->status >= 300) {
        throw std::runtime_error("codex app-server session open failed");
    }
    const auto json = nlohmann::json::parse(response->body);
    if (!json.contains("session_id") || !json.at("session_id").is_string()) {
        throw std::runtime_error("codex app-server session response missing session_id");
    }
    return json.at("session_id").get<std::string>();
}

std::string CodexAppServerAutoDevAdapter::run_turn(
    const std::string& session_id,
    const std::string& prompt,
    int* exit_code,
    std::vector<std::string>* events) const {
    auto client = HttpClientFor(base_url_);
    const auto body = nlohmann::json{
        {"session_id", session_id},
        {"prompt", prompt},
        {"event_stream", "ndjson"},
    }.dump();
    const auto response = client.Post(("/sessions/" + session_id + "/turns").c_str(), body, "application/json");
    if (!response || response->status < 200 || response->status >= 300) {
        throw std::runtime_error("codex app-server turn failed");
    }
    const auto json = nlohmann::json::parse(response->body);
    if (exit_code != nullptr) {
        *exit_code = json.value("exit_code", 0);
    }
    if (events != nullptr) {
        events->clear();
        if (json.contains("events") && json.at("events").is_array()) {
            for (const auto& event : json.at("events")) {
                events->push_back(event.is_string() ? event.get<std::string>() : event.dump());
            }
        }
    }
    return json.value("output", std::string{});
}

}  // namespace agentos
