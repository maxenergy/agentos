#include "hosts/agents/codex_cli_agent.hpp"

#include "utils/command_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/path_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>

namespace agentos {

namespace {

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return "";
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

}  // namespace

CodexCliAgent::CodexCliAgent(const CliHost& cli_host, std::filesystem::path workspace_root)
    : cli_host_(cli_host),
      workspace_root_(NormalizeWorkspaceRoot(std::move(workspace_root))) {}

AgentProfile CodexCliAgent::profile() const {
    return {
        .agent_name = "codex_cli",
        .version = "0.1.0",
        .description = "Adapter for Codex CLI non-interactive execution.",
        .capabilities = {
            {"code_reasoning", 95},
            {"planning", 85},
            {"patch_generation", 80},
        },
        .supports_session = false,
        .supports_streaming = false,
        .supports_patch = true,
        .supports_subagents = false,
        .supports_network = true,
        .cost_tier = "model-dependent",
        .latency_tier = "medium",
        .risk_level = "medium",
    };
}

bool CodexCliAgent::healthy() const {
    return CommandExists("codex") || CommandExists("codex.cmd");
}

std::string CodexCliAgent::start_session(const std::string& session_config_json) {
    (void)session_config_json;
    const auto next_id = session_counter_.fetch_add(1) + 1;
    return "codex-cli-session-" + std::to_string(next_id);
}

void CodexCliAgent::close_session(const std::string& session_id) {
    (void)session_id;
}

AgentResult CodexCliAgent::run_task(const AgentTask& task) {
    if (!healthy()) {
        return {
            .success = false,
            .error_code = "AgentUnavailable",
            .error_message = "codex CLI was not found on PATH",
        };
    }

    const auto workspace_path = NormalizeWorkspaceRoot(task.workspace_path.empty() ? workspace_root_ : task.workspace_path);
    if (!IsPathInsideWorkspace(workspace_root_, workspace_path)) {
        return {
            .success = false,
            .error_code = "WorkspaceEscapeDenied",
            .error_message = "agent workspace must stay inside the configured root",
        };
    }

    const auto output_dir = workspace_path / "runtime" / "agents" / "codex_cli";
    std::filesystem::create_directories(output_dir);
    const auto output_file = output_dir / (SafeFileStem(task.task_id) + "_last_message.txt");

    CliSpec spec{
        .name = "codex_cli_agent",
        .description = "Run Codex CLI in non-interactive mode.",
        .binary = "codex",
        .args_template = {
            "exec",
            "--sandbox",
            "read-only",
            "--skip-git-repo-check",
            "--color",
            "never",
            "--output-last-message",
            "{{output_file}}",
            "-C",
            "{{workspace_path}}",
            "{{prompt}}",
        },
        .required_args = {"workspace_path", "output_file", "prompt"},
        .input_schema_json = R"({"type":"object","required":["prompt"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"filesystem.read", "process.spawn", "network.access"},
        .timeout_ms = task.timeout_ms > 0 ? task.timeout_ms : 120000,
        .output_limit_bytes = 1024 * 1024,
        .env_allowlist = {"USERPROFILE", "HOMEDRIVE", "HOMEPATH", "HOME", "APPDATA", "LOCALAPPDATA", "XDG_CONFIG_HOME", "CODEX_HOME"},
    };

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {
            {"workspace_path", workspace_path.string()},
            {"output_file", output_file.string()},
            {"prompt", BuildPrompt(task)},
        },
        .workspace_path = workspace_path,
    });

    auto final_message = ReadTextFile(output_file);
    if (final_message.empty()) {
        final_message = result.stdout_text;
    }

    return {
        .success = result.success,
        .summary = result.success ? final_message : "Codex CLI task failed.",
        .structured_output_json = MakeJsonObject({
            {"agent", QuoteJson("codex_cli")},
            {"command", QuoteJson(result.command_display)},
            {"exit_code", NumberAsJson(result.exit_code)},
            {"stdout", QuoteJson(result.stdout_text)},
            {"stderr", QuoteJson(result.stderr_text)},
            {"last_message_file", QuoteJson(output_file.string())},
        }),
        .artifacts = {
            AgentArtifact{
                .type = "text",
                .uri = output_file.string(),
                .content = final_message,
                .metadata_json = MakeJsonObject({{"source", QuoteJson("codex_cli")}}),
            },
        },
        .duration_ms = result.duration_ms,
        .estimated_cost = 0.0,
        .error_code = result.error_code,
        .error_message = result.error_message,
    };
}

AgentResult CodexCliAgent::run_task_in_session(const std::string& session_id, const AgentTask& task) {
    auto result = run_task(task);
    if (!session_id.empty()) {
        result.summary = "[" + session_id + "] " + result.summary;
    }
    return result;
}

bool CodexCliAgent::cancel(const std::string& task_id) {
    (void)task_id;
    return false;
}

std::string CodexCliAgent::BuildPrompt(const AgentTask& task) {
    std::ostringstream prompt;
    prompt
        << "You are running as a secondary expert agent inside AgentOS.\n"
        << "Operate in read-only mode unless the objective explicitly asks for a patch.\n"
        << "Return a concise structured summary with findings, suggested next steps, and risks.\n\n"
        << "Task id: " << task.task_id << "\n"
        << "Task type: " << task.task_type << "\n"
        << "Objective: " << task.objective << "\n"
        << "Workspace: " << task.workspace_path << "\n";

    if (!task.context_json.empty()) {
        prompt << "Context JSON: " << task.context_json << "\n";
    }
    if (!task.constraints_json.empty()) {
        prompt << "Constraints JSON: " << task.constraints_json << "\n";
    }

    return prompt.str();
}

std::string CodexCliAgent::SafeFileStem(std::string value) {
    std::replace_if(value.begin(), value.end(), [](const unsigned char ch) {
        return !std::isalnum(ch) && ch != '-' && ch != '_';
    }, '_');

    if (value.empty()) {
        return "task";
    }
    return value;
}

}  // namespace agentos
