#include "core/execution/agent_event_runtime_store.hpp"

#include <fstream>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

std::string ReadTextFileLocal(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace

AgentEventRuntimeStore::AgentEventRuntimeStore(std::filesystem::path workspace)
    : workspace_(std::move(workspace)) {}

std::optional<std::filesystem::path> AgentEventRuntimeStore::find_task_dir(const std::string& task_id) const {
    const auto agents_root = workspace_ / "runtime" / "agents";
    std::error_code ec;
    if (!std::filesystem::exists(agents_root, ec) || !std::filesystem::is_directory(agents_root, ec)) {
        return std::nullopt;
    }
    for (const auto& entry : std::filesystem::directory_iterator(agents_root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto candidate = entry.path() / task_id;
        if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<AgentEventStatusSummary> AgentEventRuntimeStore::read_latest_status(
    const std::filesystem::path& task_dir) const {
    std::error_code ec;
    if (!std::filesystem::exists(task_dir, ec)) {
        return std::nullopt;
    }
    std::filesystem::path latest_status;
    std::filesystem::file_time_type latest_time{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(task_dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().filename() != "status.json") {
            continue;
        }
        const auto write_time = entry.last_write_time(ec);
        if (latest_status.empty() || write_time > latest_time) {
            latest_status = entry.path();
            latest_time = write_time;
        }
    }
    if (latest_status.empty()) {
        return std::nullopt;
    }

    AgentEventStatusSummary summary;
    summary.status_file = latest_status;
    try {
        const auto status = nlohmann::json::parse(ReadTextFileLocal(latest_status));
        summary.state = status.value("state", std::string("unknown"));
        if (status.contains("elapsed_ms") && status["elapsed_ms"].is_number_integer()) {
            summary.elapsed_ms = status["elapsed_ms"].get<int>();
        }
        if (status.contains("heartbeat_count") && status["heartbeat_count"].is_number_integer()) {
            summary.heartbeat_count = status["heartbeat_count"].get<int>();
        }
        if (status.contains("wait_policy") && status["wait_policy"].is_string()) {
            summary.wait_policy = status["wait_policy"].get<std::string>();
        }
    } catch (const std::exception&) {
        summary.state = "unknown";
    }
    return summary;
}

std::string FormatAgentEventStatusSummary(const AgentEventStatusSummary& summary) {
    std::ostringstream out;
    out << (summary.state.empty() ? "unknown" : summary.state);
    if (summary.elapsed_ms >= 0) {
        out << " elapsed=" << (summary.elapsed_ms / 1000) << "s";
    }
    if (summary.heartbeat_count >= 0) {
        out << " heartbeat=" << summary.heartbeat_count;
    }
    if (!summary.wait_policy.empty()) {
        out << " policy=" << summary.wait_policy;
    }
    out << " status=" << summary.status_file.string();
    return out.str();
}

}  // namespace agentos
