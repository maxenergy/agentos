#include "cli/serve_commands.hpp"

#include "utils/signal_cancellation.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace agentos {

namespace {

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string MakeTaskId(const std::string& prefix) {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return prefix + "-" + std::to_string(value);
}

std::vector<std::string> SplitCommaList(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument.rfind("--", 0) == 0) {
            argument = argument.substr(2);
        }
        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        options[argument.substr(0, separator)] = argument.substr(separator + 1);
    }
    return options;
}

// Convert a TaskRunResult to a JSON response.
nlohmann::ordered_json TaskRunResultToJson(const TaskRunResult& result) {
    nlohmann::ordered_json j;
    j["success"] = result.success;
    j["from_cache"] = result.from_cache;
    j["route"] = route_target_kind_name(result.route_kind) + " -> " + result.route_target;
    j["summary"] = result.summary;

    if (!result.output_json.empty()) {
        // Try to parse output as JSON object; fall back to string.
        try {
            j["output"] = nlohmann::json::parse(result.output_json);
        } catch (const nlohmann::json::exception&) {
            j["output"] = result.output_json;
        }
    }
    if (!result.error_code.empty()) {
        j["error_code"] = result.error_code;
    }
    if (!result.error_message.empty()) {
        j["error_message"] = result.error_message;
    }
    j["duration_ms"] = result.duration_ms;
    return j;
}

// Build a TaskRequest from a JSON body.
TaskRequest BuildTaskFromJson(const nlohmann::json& body,
                               const std::filesystem::path& workspace) {
    TaskRequest task{
        .task_id = MakeTaskId("api"),
        .task_type = body.value("task_type", ""),
        .objective = body.value("objective", std::string("API task: ") + body.value("task_type", "")),
        .workspace_path = workspace,
    };

    if (body.contains("inputs") && body["inputs"].is_object()) {
        for (auto& [key, value] : body["inputs"].items()) {
            task.inputs[key] = value.is_string() ? value.get<std::string>() : value.dump();
        }
    }
    if (body.contains("target")) {
        task.preferred_target = body["target"].get<std::string>();
    }
    if (body.contains("idempotency_key")) {
        task.idempotency_key = body["idempotency_key"].get<std::string>();
    }
    if (body.contains("user_id")) {
        task.user_id = body["user_id"].get<std::string>();
    }
    if (body.contains("timeout_ms")) {
        task.timeout_ms = body["timeout_ms"].get<int>();
    }
    if (body.contains("allow_network")) {
        task.allow_network = body["allow_network"].get<bool>();
    }
    if (body.contains("allow_high_risk")) {
        task.allow_high_risk = body["allow_high_risk"].get<bool>();
    }
    if (body.contains("approval_id")) {
        task.approval_id = body["approval_id"].get<std::string>();
    }
    if (body.contains("profile") && body["profile"].is_string()) {
        task.auth_profile = body["profile"].get<std::string>();
    } else if (body.contains("auth_profile") && body["auth_profile"].is_string()) {
        task.auth_profile = body["auth_profile"].get<std::string>();
    }
    if (body.contains("permission_grants")) {
        for (const auto& grant : body["permission_grants"]) {
            task.permission_grants.push_back(grant.get<std::string>());
        }
    }

    return task;
}

// Add CORS headers for local development.
void SetCorsHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

}  // namespace

// ── Entry point ─────────────────────────────────────────────────────────────

int RunServeCommand(
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    AgentLoop& loop,
    MemoryManager& memory_manager,
    Scheduler& scheduler,
    AuditLogger& audit_logger,
    const std::filesystem::path& workspace,
    const int argc,
    char* argv[]) {

    const auto options = ParseOptionsFromArgs(argc, argv, 2);
    const auto host = options.count("host") ? options.at("host") : std::string("127.0.0.1");
    int port = 18080;
    if (options.count("port")) {
        try { port = std::stoi(options.at("port")); } catch (...) {}
    }

    httplib::Server server;

    // ── CORS preflight ──────────────────────────────────────────────────
    server.Options(".*", [](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        res.status = 204;
    });

    // ── GET /api/health ─────────────────────────────────────────────────
    server.Get("/api/health", [](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        nlohmann::ordered_json j;
        j["status"] = "ok";
        res.set_content(j.dump(), "application/json");
    });

    // ── GET /api/skills ─────────────────────────────────────────────────
    server.Get("/api/skills", [&skill_registry](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        nlohmann::ordered_json j = nlohmann::json::array();
        for (const auto& m : skill_registry.list()) {
            nlohmann::ordered_json s;
            s["name"] = m.name;
            s["version"] = m.version;
            s["description"] = m.description;
            s["risk_level"] = m.risk_level;
            s["idempotent"] = m.idempotent;
            s["timeout_ms"] = m.timeout_ms;
            j.push_back(std::move(s));
        }
        nlohmann::ordered_json resp;
        resp["skills"] = std::move(j);
        resp["count"] = skill_registry.list().size();
        res.set_content(resp.dump(2), "application/json");
    });

    // ── GET /api/agents ─────────────────────────────────────────────────
    server.Get("/api/agents", [&agent_registry](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        nlohmann::ordered_json j = nlohmann::json::array();
        for (const auto& prof : agent_registry.list_profiles()) {
            nlohmann::ordered_json a;
            a["name"] = prof.agent_name;
            a["version"] = prof.version;
            a["description"] = prof.description;
            a["supports_streaming"] = prof.supports_streaming;
            a["supports_session"] = prof.supports_session;
            a["cost_tier"] = prof.cost_tier;
            a["latency_tier"] = prof.latency_tier;
            a["risk_level"] = prof.risk_level;
            j.push_back(std::move(a));
        }
        nlohmann::ordered_json resp;
        resp["agents"] = std::move(j);
        resp["count"] = agent_registry.list_profiles().size();
        res.set_content(resp.dump(2), "application/json");
    });

    // ── GET /api/status ─────────────────────────────────────────────────
    server.Get("/api/status", [&](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        nlohmann::ordered_json j;
        j["workspace"] = workspace.string();
        j["skills"] = skill_registry.list().size();
        j["agents"] = agent_registry.list_profiles().size();
        j["scheduled_tasks"] = scheduler.list().size();
        j["task_log_entries"] = memory_manager.task_log().size();
        j["workflow_candidates"] = memory_manager.workflow_candidates().size();
        j["audit_log"] = audit_logger.log_path().string();
        res.set_content(j.dump(2), "application/json");
    });

    // ── POST /api/run ───────────────────────────────────────────────────
    server.Post("/api/run", [&](const httplib::Request& req, httplib::Response& res) {
        SetCorsHeaders(res);
        try {
            const auto body = nlohmann::json::parse(req.body);

            if (!body.contains("task_type") || !body["task_type"].is_string() ||
                body["task_type"].get<std::string>().empty()) {
                nlohmann::ordered_json err;
                err["error"] = "task_type is required";
                res.status = 400;
                res.set_content(err.dump(), "application/json");
                return;
            }

            const auto task = BuildTaskFromJson(body, workspace);
            auto task_cancel = InstallSignalCancellation();
            const auto result = loop.run(task, std::move(task_cancel));
            res.set_content(TaskRunResultToJson(result).dump(2), "application/json");
        } catch (const nlohmann::json::exception& e) {
            nlohmann::ordered_json err;
            err["error"] = std::string("Invalid JSON: ") + e.what();
            res.status = 400;
            res.set_content(err.dump(), "application/json");
        } catch (const std::exception& e) {
            nlohmann::ordered_json err;
            err["error"] = std::string("Internal error: ") + e.what();
            res.status = 500;
            res.set_content(err.dump(), "application/json");
        }
    });

    // ── GET /api/schedule/list ──────────────────────────────────────────
    server.Get("/api/schedule/list", [&scheduler](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        nlohmann::ordered_json j = nlohmann::json::array();
        for (const auto& task : scheduler.list()) {
            nlohmann::ordered_json t;
            t["schedule_id"] = task.schedule_id;
            t["enabled"] = task.enabled;
            t["task_type"] = task.task.task_type;
            t["objective"] = task.task.objective;
            t["next_run_epoch_ms"] = task.next_run_epoch_ms;
            t["interval_seconds"] = task.interval_seconds;
            t["run_count"] = task.run_count;
            t["max_runs"] = task.max_runs;
            j.push_back(std::move(t));
        }
        nlohmann::ordered_json resp;
        resp["tasks"] = std::move(j);
        resp["count"] = scheduler.list().size();
        res.set_content(resp.dump(2), "application/json");
    });

    // ── GET /api/memory/stats ──────────────────────────────────────────
    server.Get("/api/memory/stats", [&memory_manager](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        nlohmann::ordered_json j;

        nlohmann::ordered_json skills = nlohmann::json::object();
        for (const auto& [name, stats] : memory_manager.skill_stats()) {
            nlohmann::ordered_json s;
            s["total_calls"] = stats.total_calls;
            s["success_calls"] = stats.success_calls;
            s["avg_latency_ms"] = stats.avg_latency_ms;
            skills[name] = std::move(s);
        }
        j["skill_stats"] = std::move(skills);

        nlohmann::ordered_json agents = nlohmann::json::object();
        for (const auto& [name, stats] : memory_manager.agent_stats()) {
            nlohmann::ordered_json a;
            a["total_runs"] = stats.total_runs;
            a["success_runs"] = stats.success_runs;
            a["avg_duration_ms"] = stats.avg_duration_ms;
            agents[name] = std::move(a);
        }
        j["agent_stats"] = std::move(agents);

        j["task_log_entries"] = memory_manager.task_log().size();
        j["workflow_candidates"] = memory_manager.workflow_candidates().size();

        res.set_content(j.dump(2), "application/json");
    });

    // ── Start server ────────────────────────────────────────────────────
    auto cancel = InstallSignalCancellation();

#ifdef _WIN32
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    std::cout
        << "\n"
        << "  +==================================================+\n"
        << "  |             AgentOS HTTP API Server              |\n"
        << "  +==================================================+\n"
        << "\n"
        << "  Listening on: http://" << host << ":" << port << "\n"
        << "  Workspace:    " << workspace.string() << "\n"
        << "\n"
        << "  Endpoints:\n"
        << "    GET  /api/health          Health check\n"
        << "    GET  /api/skills          List skills\n"
        << "    GET  /api/agents          List agents\n"
        << "    GET  /api/status          Runtime status\n"
        << "    POST /api/run             Execute a task\n"
        << "    GET  /api/schedule/list   List scheduled tasks\n"
        << "    GET  /api/memory/stats    Memory statistics\n"
        << "\n"
        << "  Press Ctrl-C to stop the server.\n"
        << "\n";

    // Start a background thread that watches for Ctrl-C and stops the server.
    std::thread shutdown_thread([&server, &cancel]() {
        if (cancel) {
            cancel->wait_for_cancel(std::chrono::hours(24 * 365));
            server.stop();
        }
    });
    shutdown_thread.detach();

    if (!server.listen(host, port)) {
        std::cerr << "Failed to start server on " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "\nServer stopped.\n";
    return 0;
}

}  // namespace agentos
