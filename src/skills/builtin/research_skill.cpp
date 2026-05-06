#include "skills/builtin/research_skill.hpp"

#include "core/audit/audit_logger.hpp"
#include "core/execution/task_wait_policy.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/registry/agent_registry.hpp"
#include "utils/signal_cancellation.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

std::string MakeResearchTaskId() {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return std::string("research-") + std::to_string(value);
}

std::string ReadEnvVarLocal(const std::string& name) {
    if (name.empty()) {
        return {};
    }
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, name.c_str()) == 0 && raw != nullptr) {
        std::string value(raw, size > 0 ? size - 1 : 0);
        std::free(raw);
        return value;
    }
    return {};
#else
    if (const char* raw = std::getenv(name.c_str()); raw != nullptr) {
        return raw;
    }
    return {};
#endif
}

std::string ResolveResearchTargetLocal(const AgentRegistry& agent_registry) {
    const auto configured = ReadEnvVarLocal("AGENTOS_RESEARCH_TARGET");
    if (!configured.empty()) {
        const auto agent = agent_registry.find(configured);
        if (agent && agent->healthy() && agent->profile().supports_network) {
            return configured;
        }
    }
    const auto codex = agent_registry.find("codex_cli");
    if (codex && codex->healthy() && codex->profile().supports_network) {
        return "codex_cli";
    }
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto agent = agent_registry.find(profile.agent_name);
        if (agent && agent->healthy() && profile.supports_network) {
            return profile.agent_name;
        }
    }
    return {};
}

std::string ShortenForConsole(const std::string& text, std::size_t max_chars = 240) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "...";
}

std::string AgentEventKindName(const AgentEvent::Kind kind) {
    switch (kind) {
    case AgentEvent::Kind::SessionInit:    return "session_init";
    case AgentEvent::Kind::TextDelta:      return "text_delta";
    case AgentEvent::Kind::Thinking:       return "thinking";
    case AgentEvent::Kind::ToolUseStart:   return "tool_start";
    case AgentEvent::Kind::ToolUseResult:  return "tool_result";
    case AgentEvent::Kind::Status:         return "status";
    case AgentEvent::Kind::CompactBoundary:return "compact_boundary";
    case AgentEvent::Kind::Usage:          return "usage";
    case AgentEvent::Kind::Final:          return "final";
    case AgentEvent::Kind::Error:          return "error";
    }
    return "unknown";
}

void AppendAgentEventJsonl(const std::filesystem::path& path,
                           const std::string& task_id,
                           const AgentEvent& event) {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::ordered_json json;
    json["task_id"] = task_id;
    json["kind"] = AgentEventKindName(event.kind);
    json["fields"] = event.fields;
    json["payload_text"] = event.payload_text;
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output << json.dump() << '\n';
}

void WriteAgentTaskStatus(const std::filesystem::path& path,
                          const TaskRequest& task,
                          const std::string& target,
                          const std::string& state,
                          const std::string& events_file,
                          const TaskRunResult* result = nullptr) {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::ordered_json json;
    json["task_id"] = task.task_id;
    json["target"] = target;
    json["state"] = state;
    json["objective"] = task.objective;
    json["workspace"] = task.workspace_path.string();
    json["events_file"] = events_file;
    if (const auto it = task.inputs.find("wait_policy"); it != task.inputs.end()) {
        json["wait_policy"] = it->second;
    }
    if (const auto it = task.inputs.find("idle_timeout_ms"); it != task.inputs.end()) {
        json["idle_timeout_ms"] = it->second;
    }
    if (const auto it = task.inputs.find("soft_deadline_ms"); it != task.inputs.end()) {
        json["soft_deadline_ms"] = it->second;
    }
    if (const auto it = task.inputs.find("hard_deadline_ms"); it != task.inputs.end()) {
        json["hard_deadline_ms"] = it->second;
    }
    if (result != nullptr) {
        json["success"] = result->success;
        json["duration_ms"] = result->duration_ms;
        json["summary"] = result->summary;
        json["error_code"] = result->error_code;
        json["error_message"] = result->error_message;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
}

std::string BuildResearchObjective(const std::string& line, const std::string& runtime_guide) {
    std::ostringstream objective;
    objective
        << "Research the user's request using current internet sources when needed.\n\n"
        << "USER REQUEST:\n"
        << line << "\n\n";
    if (!runtime_guide.empty()) {
        objective
            << "AGENTOS RUNTIME CONTEXT:\n"
            << runtime_guide << "\n";
    }
    objective
        << "RESEARCH REQUIREMENTS:\n"
        << "- Do not answer only from memory when the request names external projects, tools, or current integration details.\n"
        << "- Use available network/search/curl capabilities to find authoritative sources.\n"
        << "- Include source URLs for the facts you rely on.\n"
        << "- If a project or tool cannot be confidently identified, say so and list what you checked.\n"
        << "- Explain how the external tool could integrate with this AgentOS runtime using commands, skills, adapters, or workflows.\n"
        << "- Do not edit files for this research task.\n";
    return objective.str();
}

}  // namespace

SkillManifest ResearchSkill::manifest() const {
    return {
        .name = "research_request",
        .version = "0.1.0",
        .description = "Run a free-form research request through a network-capable agent (codex_cli by default).",
        .capabilities = {"agent_orchestration", "research", "network"},
        .input_schema_json = R"({"type":"object","properties":{"objective":{"type":"string"},"interactive":{"type":"string"}},"required":["objective"]})",
        .output_schema_json = R"({"type":"object","required":["accepted","task_dir","status"]})",
        .risk_level = "medium",
        .permissions = {"agent.dispatch", "network.access"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = 0,
    };
}

bool ResearchSkill::healthy() const {
    return true;
}

SkillResult ResearchSkill::execute(const SkillCall& call) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto duration_ms = [&started_at]() {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at).count());
    };

    const auto maybe_objective = call.get_arg("objective");
    if (!maybe_objective.has_value() || maybe_objective->empty()) {
        return {false, "", "SchemaValidationFailed", "objective is required", duration_ms()};
    }
    const std::string line = *maybe_objective;
    const bool interactive = call.get_arg("interactive").value_or("") == "true";

    const auto target = ResolveResearchTargetLocal(agent_registry_);
    if (target.empty()) {
        if (interactive) {
            std::cerr
                << "This looks like a research task, but no healthy network-capable agent is available.\n"
                << "Configure codex_cli or set AGENTOS_RESEARCH_TARGET to a healthy network-capable agent.\n";
        }
        return {false, "", "AgentUnavailable",
                "no healthy network-capable agent is available; configure codex_cli or set AGENTOS_RESEARCH_TARGET",
                duration_ms()};
    }

    const std::filesystem::path workspace = call.workspace_id.empty()
        ? workspace_root_
        : std::filesystem::path(call.workspace_id);

    TaskRequest task{
        .task_id = MakeResearchTaskId(),
        .task_type = "analysis",
        .objective = BuildResearchObjective(line, runtime_guide_),
        .workspace_path = workspace,
    };
    task.preferred_target = target;
    task.allow_network = true;
    task.inputs["research_intent"] = "internet_research";
    task.inputs["allow_writes"] = "false";
    ApplyTaskWaitPolicy(task, ResolveTaskWaitPolicy(TaskWaitPolicyKind::research));
    if (!runtime_guide_.empty()) {
        task.inputs["runtime_usage_guide"] = runtime_guide_;
    }

    const auto task_dir = workspace / "runtime" / "agents" / target / task.task_id;
    const auto events_file = task_dir / "events.jsonl";
    const auto status_file = task_dir / "status.json";
    WriteAgentTaskStatus(status_file, task, target, "running", events_file.string());

    if (interactive) {
        std::cout << "(routing research task to " << target
                  << " - this can take 10-60s; Ctrl-C to cancel)\n";
        std::cout << "(research task dir: " << task_dir.string() << ")\n";
        std::cout << "(research events: " << events_file.string() << ")\n";
    }

    auto task_cancel = InstallSignalCancellation();
    auto on_event = [&](const AgentEvent& event) -> bool {
        AppendAgentEventJsonl(events_file, task.task_id, event);
        if (interactive && event_printer_) {
            event_printer_(event);
        }
        return true;
    };

    const auto result = loop_.run(task, std::move(task_cancel), on_event);
    WriteAgentTaskStatus(status_file, task, target,
                         result.success ? "completed" : "failed",
                         events_file.string(), &result);

    nlohmann::ordered_json output;
    output["task_dir"] = task_dir.string();
    output["target"] = target;
    output["accepted"] = result.success;
    output["status"] = result.success ? "completed" : "failed";
    output["summary"] = result.summary;
    output["duration_ms"] = result.duration_ms;

    if (interactive) {
        if (result.success) {
            std::cout << result.summary;
            if (!result.summary.empty() && result.summary.back() != '\n') {
                std::cout << '\n';
            }
            std::cout << "(via " << target << ", " << result.duration_ms << "ms)\n";
            std::cout << "Research task dir: " << task_dir.string() << "\n\n";
        } else {
            std::cerr << "research task failed (" << target << "): "
                      << (result.error_code.empty() ? "<no error_code>" : result.error_code);
            if (!result.error_message.empty()) {
                std::cerr << " - " << result.error_message;
            }
            std::cerr << "\naudit_log: " << audit_logger_.log_path().string() << '\n';
        }
    }

    SkillResult skill_result;
    skill_result.success = result.success;
    skill_result.json_output = output.dump();
    skill_result.duration_ms = duration_ms();
    if (!skill_result.success) {
        skill_result.error_code = result.error_code.empty() ? "ExternalProcessFailed" : result.error_code;
        skill_result.error_message = result.error_message;
    }
    return skill_result;
}

}  // namespace agentos
