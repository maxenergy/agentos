#include "skills/builtin/development_skill.hpp"

#include "core/audit/audit_logger.hpp"
#include "core/execution/task_wait_policy.hpp"
#include "core/loop/agent_loop.hpp"
#include "core/registry/agent_registry.hpp"
#include "utils/signal_cancellation.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

std::string MakeDevTaskId() {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return std::string("dev-") + std::to_string(value);
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

std::string LowerForRouting(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsDevelopmentCapable(const AgentProfile& profile) {
    if (profile.supports_patch) {
        return true;
    }
    for (const auto& capability : profile.capabilities) {
        const auto name = LowerForRouting(capability.name);
        if (name == "code_reasoning" || name == "patch_generation") {
            return true;
        }
    }
    return false;
}

std::vector<std::string> SignificantAgentTerms(const AgentProfile& profile) {
    std::vector<std::string> terms;
    auto add = [&](std::string term) {
        term = LowerForRouting(std::move(term));
        if (term.size() < 4) {
            return;
        }
        static const std::vector<std::string> ignored = {
            "agent", "adapter", "model", "through", "authenticated", "provider",
            "layer", "local", "code", "task", "tasks", "analysis", "planning",
            "chat", "main", "with", "from", "into", "using",
        };
        if (std::find(ignored.begin(), ignored.end(), term) != ignored.end()) {
            return;
        }
        if (std::find(terms.begin(), terms.end(), term) == terms.end()) {
            terms.push_back(std::move(term));
        }
    };

    std::string token;
    auto scan = [&](const std::string& text) {
        for (const unsigned char ch : text) {
            if (std::isalnum(ch)) {
                token.push_back(static_cast<char>(ch));
            } else {
                add(std::move(token));
                token.clear();
            }
        }
        add(std::move(token));
        token.clear();
    };

    scan(profile.agent_name);
    scan(profile.description);
    for (const auto& capability : profile.capabilities) {
        scan(capability.name);
    }
    return terms;
}

bool MentionsAgent(const std::string& lower_line, const AgentProfile& profile) {
    const auto lower_name = LowerForRouting(profile.agent_name);
    if (!lower_name.empty() && lower_line.find(lower_name) != std::string::npos) {
        return true;
    }
    std::string spaced_name = lower_name;
    std::replace(spaced_name.begin(), spaced_name.end(), '_', ' ');
    if (spaced_name != lower_name && lower_line.find(spaced_name) != std::string::npos) {
        return true;
    }
    const auto terms = SignificantAgentTerms(profile);
    return std::any_of(terms.begin(), terms.end(), [&](const std::string& term) {
        return lower_line.find(term) != std::string::npos;
    });
}

std::string SafeAgentPathComponent(std::string value) {
    if (value.empty()) {
        return "agent";
    }
    for (char& ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!std::isalnum(uch) && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    return value;
}

std::string ResolveDevTargetLocal(const AgentRegistry& agent_registry, const std::string& objective) {
    const auto configured = ReadEnvVarLocal("AGENTOS_DEV_TARGET");
    if (!configured.empty()) {
        const auto agent = agent_registry.find(configured);
        if (agent && agent->healthy() && IsDevelopmentCapable(agent->profile())) {
            return configured;
        }
    }

    const auto lower_objective = LowerForRouting(objective);
    struct Candidate {
        std::string name;
        int score = 0;
    };
    std::vector<Candidate> mentioned;
    bool mentioned_but_unavailable = false;
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto agent = agent_registry.find(profile.agent_name);
        if (!IsDevelopmentCapable(profile) || !MentionsAgent(lower_objective, profile)) {
            continue;
        }
        if (!agent || !agent->healthy()) {
            mentioned_but_unavailable = true;
            continue;
        }
        int score = profile.supports_patch ? 20 : 0;
        if (!profile.agent_name.empty() &&
            lower_objective.find(LowerForRouting(profile.agent_name)) != std::string::npos) {
            score += 100;
        } else {
            score += 60;
        }
        mentioned.push_back({profile.agent_name, score});
    }
    if (!mentioned.empty()) {
        std::stable_sort(mentioned.begin(), mentioned.end(), [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.score > rhs.score;
        });
        return mentioned.front().name;
    }
    if (mentioned_but_unavailable) {
        return {};
    }

    const auto codex = agent_registry.find("codex_cli");
    if (codex && codex->healthy()) {
        return "codex_cli";
    }
    for (const auto& profile : agent_registry.list_profiles()) {
        const auto agent = agent_registry.find(profile.agent_name);
        if (agent && agent->healthy() && IsDevelopmentCapable(profile)) {
            return profile.agent_name;
        }
    }
    return {};
}

int ResolveMaxDevelopmentAttempts() {
    constexpr int kDefault = 3;
    constexpr int kUpper = 8;
    const auto configured = ReadEnvVarLocal("AGENTOS_DEV_MAX_ATTEMPTS");
    if (configured.empty()) {
        return kDefault;
    }
    try {
        const int parsed = std::stoi(configured);
        return std::clamp(parsed, 1, kUpper);
    } catch (const std::exception&) {
        return kDefault;
    }
}

std::string ShortenForConsole(const std::string& text, std::size_t max_chars = 240) {
    if (text.size() <= max_chars) {
        return text;
    }
    return text.substr(0, max_chars) + "...";
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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
                          const TaskRunResult* result = nullptr,
                          const int elapsed_ms = -1,
                          const int heartbeat_count = -1) {
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
    if (elapsed_ms >= 0) {
        json["elapsed_ms"] = elapsed_ms;
    }
    if (heartbeat_count >= 0) {
        json["heartbeat_count"] = heartbeat_count;
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

std::string ReadLocalTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

struct DevelopmentAcceptance {
    std::vector<std::string> changed_files;
    std::vector<std::string> deliverable_files;
    std::vector<std::string> warnings;
    bool repairable = true;
    bool accepted = false;
    std::string status = "failed";
};

struct TaskContract {
    std::string instructions;
};

struct DevelopmentAttempt {
    int index = 0;
    std::string task_id;
    std::filesystem::path attempt_dir;
    std::filesystem::path events_file;
    std::filesystem::path status_file;
    std::filesystem::path deliverables_file;
    std::filesystem::path acceptance_file;
    TaskRunResult result;
    DevelopmentAcceptance acceptance;
};

TaskContract BuildTaskContract(const std::string& line) {
    TaskContract contract;
    contract.instructions =
        "Infer the concrete acceptance deliverables from the user's request and produce real workspace artifacts, "
        "not just prose. The primary agent will not hard-code task-specific file types; instead, you must declare "
        "the artifacts you produced in the deliverables manifest path provided by AgentOS. The manifest must list "
        "every delivered artifact path, a short description, and verification status. If you cannot create the "
        "right artifact, mark the task partial or blocked and explain the blocker.";
    (void)line;
    return contract;
}

void CollectChangedFilesFromPayload(const std::string& payload_text,
                                    std::vector<std::string>& changed_files) {
    if (payload_text.empty()) {
        return;
    }
    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(payload_text);
    } catch (const nlohmann::json::exception&) {
        return;
    }
    if (!payload.is_object() || !payload.contains("item") || !payload["item"].is_object()) {
        return;
    }
    const auto& item = payload["item"];
    if (!item.contains("type") || !item["type"].is_string() ||
        item["type"].get<std::string>() != "file_change") {
        return;
    }
    if (!item.contains("changes") || !item["changes"].is_array()) {
        return;
    }
    for (const auto& change : item["changes"]) {
        if (change.is_object() && change.contains("path") && change["path"].is_string()) {
            const auto path = change["path"].get<std::string>();
            if (std::find(changed_files.begin(), changed_files.end(), path) == changed_files.end()) {
                changed_files.push_back(path);
            }
        }
    }
}

std::string JoinLines(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (const auto& value : values) {
        out << "- " << value << "\n";
    }
    return out.str();
}

std::string JsonStringValue(const nlohmann::json& object, const std::string& key) {
    if (object.is_object() && object.contains(key) && object[key].is_string()) {
        return object[key].get<std::string>();
    }
    return {};
}

bool LooksLikeEnvironmentBlocker(const std::string& text) {
    const auto lower = ToLowerAscii(text);
    const std::vector<std::string> markers = {
        "operation not permitted",
        "permission denied",
        "not installed",
        "not discoverable",
        "missing dependency",
        "missing dependencies",
        "could not find",
        "not found",
        "sandbox",
        "缺少",
        "未安装",
        "找不到",
        "权限",
        "拒绝",
    };
    return std::any_of(markers.begin(), markers.end(), [&](const std::string& marker) {
        return lower.find(marker) != std::string::npos;
    });
}

std::string BuildRepairObjective(const std::string& original_request,
                                 const TaskContract& contract,
                                 const DevelopmentAttempt& previous_attempt) {
    std::ostringstream objective;
    objective
        << "Previous secondary-agent attempt failed primary acceptance. Repair the workspace now.\n\n"
        << "ORIGINAL USER REQUEST:\n"
        << original_request << "\n\n"
        << "ACCEPTANCE CONTRACT:\n"
        << contract.instructions << "\n\n"
        << "PRIMARY ACCEPTANCE FAILURES:\n"
        << JoinLines(previous_attempt.acceptance.warnings) << "\n";
    if (!previous_attempt.acceptance.deliverable_files.empty()) {
        objective << "PREVIOUS DECLARED DELIVERABLES:\n"
                  << JoinLines(previous_attempt.acceptance.deliverable_files) << "\n";
    }
    objective
        << "PREVIOUS SUMMARY:\n"
        << previous_attempt.result.summary << "\n\n"
        << "REQUIRED NEXT ACTION:\n"
        << "- Fix the existing workspace; do not merely explain the problem.\n"
        << "- Do not restart from scratch unless that is simpler and preserves useful existing work.\n"
        << "- Produce or update the actual artifacts required by the original request.\n"
        << "- Run reasonable verification, or clearly record any environmental blocker.\n"
        << "- Rewrite the deliverables manifest at the path provided in the new AgentOS context.\n";
    return objective.str();
}

DevelopmentAcceptance ReviewDevelopmentResult(const TaskRequest& task,
                                              const TaskRunResult& result,
                                              const std::filesystem::path& events_file,
                                              const std::filesystem::path& deliverables_file) {
    DevelopmentAcceptance review;
    std::ifstream input(events_file, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        try {
            const auto event = nlohmann::json::parse(line);
            if (event.is_object() && event.contains("payload_text") && event["payload_text"].is_string()) {
                CollectChangedFilesFromPayload(event["payload_text"].get<std::string>(), review.changed_files);
            }
        } catch (const nlohmann::json::exception&) {
        }
    }

    if (result.error_code == "Cancelled") {
        review.warnings.push_back("secondary agent was cancelled");
        review.repairable = false;
        review.accepted = false;
        review.status = "cancelled";
        return review;
    }

    if (!result.success) {
        review.warnings.push_back("secondary agent returned failure");
    }

    if (!std::filesystem::exists(deliverables_file)) {
        review.warnings.push_back("deliverables manifest was not written: " + deliverables_file.string());
    } else {
        try {
            const auto manifest = nlohmann::json::parse(ReadLocalTextFile(deliverables_file));
            if (!manifest.is_object()) {
                review.warnings.push_back("deliverables manifest is not a JSON object");
            } else {
                const nlohmann::json* artifacts = nullptr;
                if (manifest.contains("deliverables") && manifest["deliverables"].is_array()) {
                    artifacts = &manifest["deliverables"];
                } else if (manifest.contains("artifacts") && manifest["artifacts"].is_array()) {
                    artifacts = &manifest["artifacts"];
                }
                if (artifacts == nullptr || artifacts->empty()) {
                    review.warnings.push_back("deliverables manifest has no deliverables");
                } else {
                    for (const auto& artifact : *artifacts) {
                        if (!artifact.is_object() || !artifact.contains("path") || !artifact["path"].is_string()) {
                            review.warnings.push_back("deliverables manifest contains a deliverable without a string path");
                            continue;
                        }
                        const auto path_text = artifact["path"].get<std::string>();
                        const auto artifact_path = std::filesystem::path(path_text).is_absolute()
                            ? std::filesystem::path(path_text)
                            : task.workspace_path / path_text;
                        review.deliverable_files.push_back(artifact_path.string());
                        if (!std::filesystem::exists(artifact_path)) {
                            review.warnings.push_back("declared artifact does not exist: " + artifact_path.string());
                        } else if (std::filesystem::is_regular_file(artifact_path) &&
                                   std::filesystem::file_size(artifact_path) == 0) {
                            review.warnings.push_back("declared artifact is empty: " + artifact_path.string());
                        }
                        if (artifact.contains("verification") && artifact["verification"].is_string()) {
                            const auto artifact_verification =
                                ToLowerAscii(artifact["verification"].get<std::string>());
                            if (artifact_verification == "failed") {
                                review.warnings.push_back("declared artifact verification failed: " +
                                                          artifact_path.string());
                            }
                        }
                    }
                }
                if (manifest.contains("status") && manifest["status"].is_string()) {
                    const auto status = ToLowerAscii(manifest["status"].get<std::string>());
                    if (status != "complete" && status != "passed" && status != "done") {
                        review.warnings.push_back("deliverables manifest status is " + status);
                    }
                }
                if (manifest.contains("blockers") && manifest["blockers"].is_array() &&
                    !manifest["blockers"].empty()) {
                    review.warnings.push_back("deliverables manifest reported blockers");
                }
            }
            if (manifest.is_object() && manifest.contains("verification") && manifest["verification"].is_object()) {
                const auto& verification = manifest["verification"];
                if (verification.contains("status") && verification["status"].is_string()) {
                    const auto status = ToLowerAscii(verification["status"].get<std::string>());
                    if (status != "passed" && status != "not_applicable" && status != "complete") {
                        review.warnings.push_back("deliverables verification status is " + status);
                    }
                }
            } else if (manifest.is_object() && manifest.contains("verification") &&
                       manifest["verification"].is_array()) {
                for (const auto& verification : manifest["verification"]) {
                    if (verification.is_object() && verification.contains("success") &&
                        verification["success"].is_boolean() && !verification["success"].get<bool>()) {
                        review.warnings.push_back("deliverables manifest contains failed verification");
                        const auto command = JsonStringValue(verification, "command");
                        const auto notes = JsonStringValue(verification, "notes");
                        if (!command.empty()) {
                            review.warnings.push_back("failed verification command: " + command);
                        }
                        if (!notes.empty()) {
                            review.warnings.push_back("failed verification notes: " + ShortenForConsole(notes, 600));
                        }
                        if (LooksLikeEnvironmentBlocker(verification.dump())) {
                            review.repairable = false;
                            review.warnings.push_back("failed verification appears to be an environment blocker");
                        }
                        break;
                    }
                }
            }
        } catch (const std::exception& e) {
            review.warnings.push_back(std::string("failed to parse deliverables manifest: ") + e.what());
        }
    }
    if (review.changed_files.empty() && review.deliverable_files.empty()) {
        review.warnings.push_back("no file_change events or declared deliverables were observed");
    }

    const auto summary = result.summary;
    const std::vector<std::string> incomplete_markers = {
        "not complete",
        "not completed",
        "could not",
        "failed",
        "operation not permitted",
        "没有完成",
        "未完成",
        "无法",
        "拒绝",
        "没有完成本地编译验证",
    };
    for (const auto& marker : incomplete_markers) {
        if (summary.find(marker) != std::string::npos) {
            review.warnings.push_back("secondary agent reported: " + marker);
            if (LooksLikeEnvironmentBlocker(marker)) {
                review.repairable = false;
            }
        }
    }

    review.accepted = result.success && !review.deliverable_files.empty() && review.warnings.empty();
    if (review.accepted) {
        review.status = "passed";
    } else if (!review.repairable && !review.deliverable_files.empty()) {
        review.status = "blocked";
    } else {
        review.status = "failed";
    }
    return review;
}

void WriteAcceptanceReport(const std::filesystem::path& path,
                           const TaskRequest& task,
                           const TaskContract& contract,
                           const std::filesystem::path& deliverables_file,
                           const DevelopmentAcceptance& review) {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::ordered_json json;
    json["task_id"] = task.task_id;
    json["objective"] = task.objective;
    json["contract_instructions"] = contract.instructions;
    json["deliverables_manifest"] = deliverables_file.string();
    json["status"] = review.status;
    json["accepted"] = review.accepted;
    json["repairable"] = review.repairable;
    json["changed_files"] = review.changed_files;
    json["deliverable_files"] = review.deliverable_files;
    json["warnings"] = review.warnings;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
}

void WriteAggregateAcceptanceReport(const std::filesystem::path& path,
                                    const std::string& root_task_id,
                                    const std::string& objective,
                                    const TaskContract& contract,
                                    const std::vector<DevelopmentAttempt>& attempts) {
    std::filesystem::create_directories(path.parent_path());
    nlohmann::ordered_json json;
    json["task_id"] = root_task_id;
    json["objective"] = objective;
    json["contract_instructions"] = contract.instructions;
    json["status"] = attempts.empty() ? "failed" : attempts.back().acceptance.status;
    json["accepted"] = !attempts.empty() && attempts.back().acceptance.accepted;
    json["attempt_count"] = attempts.size();
    json["attempts"] = nlohmann::ordered_json::array();
    for (const auto& attempt : attempts) {
        nlohmann::ordered_json item;
        item["attempt"] = attempt.index;
        item["task_id"] = attempt.task_id;
        item["attempt_dir"] = attempt.attempt_dir.string();
        item["events_file"] = attempt.events_file.string();
        item["deliverables_manifest"] = attempt.deliverables_file.string();
        item["acceptance_report"] = attempt.acceptance_file.string();
        item["agent_success"] = attempt.result.success;
        item["status"] = attempt.acceptance.status;
        item["accepted"] = attempt.acceptance.accepted;
        item["repairable"] = attempt.acceptance.repairable;
        item["deliverable_files"] = attempt.acceptance.deliverable_files;
        item["warnings"] = attempt.acceptance.warnings;
        json["attempts"].push_back(std::move(item));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
}

}  // namespace

SkillManifest DevelopmentSkill::manifest() const {
    return {
        .name = "development_request",
        .version = "0.1.0",
        .description = "Run a free-form development request through the writable agent (codex_cli by default), with acceptance review and repair loop.",
        .capabilities = {"agent_orchestration", "development"},
        .input_schema_json = R"({"type":"object","properties":{"objective":{"type":"string"},"interactive":{"type":"string"}},"required":["objective"]})",
        .output_schema_json = R"({"type":"object","required":["accepted","attempts","task_dir","acceptance_status"]})",
        .risk_level = "medium",
        .permissions = {"agent.dispatch", "filesystem.write"},
        .supports_streaming = false,
        .idempotent = false,
        .timeout_ms = 0,
    };
}

bool DevelopmentSkill::healthy() const {
    return true;
}

SkillResult DevelopmentSkill::execute(const SkillCall& call) {
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

    const auto target = ResolveDevTargetLocal(agent_registry_, line);
    if (target.empty()) {
        if (interactive) {
            std::cerr
                << "This looks like a development task, but no development-capable agent is healthy.\n"
                << "Install/configure Codex CLI, Claude Code, or set AGENTOS_DEV_TARGET to a healthy code agent.\n"
                << "Task not executed.\n";
        }
        return {false, "", "AgentUnavailable",
                "no development-capable agent is healthy; install Codex CLI, Claude Code, or set AGENTOS_DEV_TARGET",
                duration_ms()};
    }

    const std::filesystem::path workspace = call.workspace_id.empty()
        ? workspace_root_
        : std::filesystem::path(call.workspace_id);
    const auto requested_root_task_id = call.get_arg("root_task_id").value_or("");
    const auto root_task_id = requested_root_task_id.empty() ? MakeDevTaskId() : requested_root_task_id;
    const auto task_dir = workspace / "runtime" / "agents" / SafeAgentPathComponent(target) / root_task_id;
    const auto aggregate_acceptance_file = task_dir / "acceptance.json";
    const auto contract = BuildTaskContract(line);
    const int max_attempts = ResolveMaxDevelopmentAttempts();
    const auto wait_policy = ResolveTaskWaitPolicy(TaskWaitPolicyKind::development);
    std::vector<DevelopmentAttempt> attempts;
    std::string resume_session_id;
    std::string next_objective = line;

    if (interactive) {
        std::cout << "(routing development task to " << target
                  << " - this can take 10-30s; Ctrl-C to cancel)" << std::endl;
        std::cout << "(agent task dir: " << task_dir.string() << ")\n";
    }
    auto task_cancel = InstallSignalCancellation();

    for (int attempt_index = 1; attempt_index <= max_attempts; ++attempt_index) {
        TaskRequest task{
            .task_id = root_task_id + "-attempt-" + std::to_string(attempt_index),
            .task_type = "analysis",
            .objective = next_objective,
            .workspace_path = workspace,
        };
        task.preferred_target = target;
        ApplyTaskWaitPolicy(task, wait_policy);
        task.inputs["allow_writes"] = "true";
        task.inputs["interactive_intent"] = "development";
        task.inputs["original_request"] = line;
        task.inputs["root_task_id"] = root_task_id;
        task.inputs["attempt_index"] = std::to_string(attempt_index);
        task.inputs["max_attempts"] = std::to_string(max_attempts);
        task.inputs["contract_instructions"] = contract.instructions;
        if (!resume_session_id.empty()) {
            task.inputs["resume_session_id"] = resume_session_id;
        }

        DevelopmentAttempt attempt;
        attempt.index = attempt_index;
        attempt.task_id = task.task_id;
        attempt.attempt_dir = task_dir / ("attempt-" + std::to_string(attempt_index));
        attempt.events_file = attempt.attempt_dir / "events.jsonl";
        attempt.status_file = attempt.attempt_dir / "status.json";
        attempt.acceptance_file = attempt.attempt_dir / "acceptance.json";
        attempt.deliverables_file = attempt.attempt_dir / "deliverables.json";
        task.inputs["deliverables_manifest"] = attempt.deliverables_file.string();

        WriteAgentTaskStatus(attempt.status_file, task, target, "running", attempt.events_file.string());
        if (interactive) {
            std::cout << "(attempt " << attempt_index << "/" << max_attempts
                      << " events: " << attempt.events_file.string() << ")\n";
        }

        auto on_event = [&](const AgentEvent& event) -> bool {
            AppendAgentEventJsonl(attempt.events_file, task.task_id, event);
            if (event.kind == AgentEvent::Kind::SessionInit && event.fields.contains("session_id")) {
                resume_session_id = event.fields.at("session_id");
            }
            if (interactive && event_printer_) {
                event_printer_(event);
            }
            return true;
        };

        const auto attempt_started_at = std::chrono::steady_clock::now();
        auto attempt_future = std::async(std::launch::async, [&]() {
            return loop_.run(task, task_cancel, on_event);
        });
        int heartbeat_count = 0;
        bool reported_soft_deadline = false;
        while (attempt_future.wait_for(std::chrono::milliseconds(wait_policy.heartbeat_interval_ms)) !=
               std::future_status::ready) {
            ++heartbeat_count;
            const auto elapsed_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - attempt_started_at).count());
            const bool past_soft_deadline = elapsed_ms >= wait_policy.soft_deadline_ms;
            WriteAgentTaskStatus(attempt.status_file, task, target,
                                 past_soft_deadline ? "running_soft_deadline" : "running",
                                 attempt.events_file.string(), nullptr,
                                 elapsed_ms, heartbeat_count);
            if (interactive) {
                std::cout << "(attempt " << attempt_index << "/" << max_attempts
                          << " still running on " << target
                          << ", elapsed " << (elapsed_ms / 1000)
                          << "s; status: " << attempt.status_file.string() << ")\n";
                if (past_soft_deadline && !reported_soft_deadline) {
                    std::cout << "(soft deadline reached; continuing in background while events/status remain available)\n";
                    reported_soft_deadline = true;
                }
            }
        }
        attempt.result = attempt_future.get();
        WriteAgentTaskStatus(attempt.status_file, task, target,
                             attempt.result.success ? "completed" : "failed",
                             attempt.events_file.string(), &attempt.result);
        attempt.acceptance =
            ReviewDevelopmentResult(task, attempt.result, attempt.events_file, attempt.deliverables_file);
        WriteAcceptanceReport(attempt.acceptance_file, task, contract,
                              attempt.deliverables_file, attempt.acceptance);

        if (interactive) {
            std::cout << "Primary acceptance attempt " << attempt_index << ": ";
            if (attempt.acceptance.status == "passed") {
                std::cout << "passed";
            } else if (attempt.acceptance.status == "blocked") {
                std::cout << "blocked";
            } else {
                std::cout << "needs repair";
            }
            std::cout << ".\n";
            for (const auto& warning : attempt.acceptance.warnings) {
                std::cout << "  - " << warning << '\n';
            }
        }

        attempts.push_back(std::move(attempt));
        WriteAggregateAcceptanceReport(aggregate_acceptance_file, root_task_id, line, contract, attempts);

        if (attempts.back().acceptance.accepted) {
            break;
        }
        if (!attempts.back().acceptance.repairable) {
            if (interactive) {
                if (attempts.back().acceptance.status == "cancelled") {
                    std::cout << "(development task was cancelled; repair loop stopped)\n";
                } else {
                    std::cout << "(primary agent stopped repair loop because the remaining failure appears to be an environment blocker)\n";
                }
            }
            break;
        }
        if (attempt_index < max_attempts) {
            if (interactive) {
                std::cout << "(primary agent is sending a repair request to " << target
                          << " based on acceptance failures)\n";
            }
            next_objective = BuildRepairObjective(line, contract, attempts.back());
        }
    }

    nlohmann::ordered_json output;
    output["task_dir"] = task_dir.string();
    output["aggregate_acceptance_file"] = aggregate_acceptance_file.string();
    output["target"] = target;
    output["attempts"] = static_cast<int>(attempts.size());

    if (attempts.empty()) {
        if (interactive) {
            std::cerr << "development task did not run.\n";
        }
        output["accepted"] = false;
        output["acceptance_status"] = "not_run";
        return {false, output.dump(), "ExternalProcessFailed",
                "development task did not run", duration_ms()};
    }

    const auto& final_attempt = attempts.back();
    output["accepted"] = final_attempt.acceptance.accepted;
    output["acceptance_status"] = final_attempt.acceptance.status;
    output["deliverable_files"] = final_attempt.acceptance.deliverable_files;
    output["changed_files"] = final_attempt.acceptance.changed_files;
    output["warnings"] = final_attempt.acceptance.warnings;
    output["final_summary"] = final_attempt.result.summary;

    if (interactive) {
        if (!final_attempt.result.summary.empty()) {
            std::cout << final_attempt.result.summary;
            if (final_attempt.result.summary.back() != '\n') {
                std::cout << '\n';
            }
        }
        std::cout << "(via " << target << ", " << final_attempt.result.duration_ms << "ms final attempt)\n\n";
        std::cout << "Agent task dir: " << task_dir.string() << '\n'
                  << "Acceptance report: " << aggregate_acceptance_file.string() << '\n';
        if (final_attempt.acceptance.accepted) {
            std::cout << "Primary acceptance: passed after " << attempts.size()
                      << " attempt(s) (" << final_attempt.acceptance.deliverable_files.size()
                      << " deliverable(s) verified";
            if (!final_attempt.acceptance.changed_files.empty()) {
                std::cout << ", " << final_attempt.acceptance.changed_files.size()
                          << " changed file event(s) observed";
            }
            std::cout << ").\n";
        } else if (final_attempt.acceptance.status == "blocked") {
            std::cout << "Primary acceptance: blocked after " << attempts.size() << " attempt(s).\n";
            std::cout << "Delivered artifacts are present, but local verification could not complete because of environment constraints.\n";
            std::cout << "Repair loop stopped because the remaining failure appears to require environment changes.\n";
            std::cout << "Contract: " << contract.instructions << '\n';
            for (const auto& warning : final_attempt.acceptance.warnings) {
                std::cout << "  - " << warning << '\n';
            }
        } else if (final_attempt.acceptance.status == "cancelled") {
            std::cout << "Primary acceptance: cancelled after " << attempts.size() << " attempt(s).\n";
            std::cout << "Repair loop stopped because the development task was cancelled.\n";
        } else {
            std::cout << "Primary acceptance: failed after " << attempts.size() << " attempt(s).\n";
            if (!final_attempt.acceptance.repairable) {
                std::cout << "Repair loop stopped because the remaining failure appears to require environment changes.\n";
            }
            std::cout << "Contract: " << contract.instructions << '\n';
            for (const auto& warning : final_attempt.acceptance.warnings) {
                std::cout << "  - " << warning << '\n';
            }
            if (!final_attempt.result.success) {
                std::cout << "Secondary agent error: "
                          << (final_attempt.result.error_code.empty() ? "<no error_code>" : final_attempt.result.error_code);
                if (!final_attempt.result.error_message.empty()) {
                    std::cout << " - " << final_attempt.result.error_message;
                }
                std::cout << '\n';
            }
        }
        std::cout << "Run `git status --short` from the workspace to see files the agent changed.\n\n";
    }

    (void)audit_logger_;

    SkillResult skill_result;
    skill_result.success = final_attempt.acceptance.accepted;
    skill_result.json_output = output.dump();
    skill_result.duration_ms = duration_ms();
    if (!skill_result.success) {
        if (final_attempt.acceptance.status == "blocked") {
            skill_result.error_code = "AcceptanceBlocked";
        } else if (final_attempt.acceptance.status == "cancelled") {
            skill_result.error_code = "Cancelled";
        } else {
            skill_result.error_code = "AcceptanceFailed";
        }
        skill_result.error_message = "development acceptance status=" + final_attempt.acceptance.status;
    }
    return skill_result;
}

}  // namespace agentos
