#include "cli/autodev_commands.hpp"

#include "autodev/autodev_execution_adapter.hpp"
#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_state_store.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

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

void PrintUsage() {
    std::cerr
        << "Usage:\n"
        << "  agentos autodev submit target_repo_path=<path> objective=<text> [skill_pack_path=<path>] [isolation_mode=git_worktree|in_place] [allow_dirty_target=true|false]\n"
        << "  agentos autodev status job_id=<job_id>\n"
        << "  agentos autodev summary job_id=<job_id>\n"
        << "  agentos autodev prepare-workspace job_id=<job_id>\n"
        << "  agentos autodev load-skill-pack job_id=<job_id> [skill_pack_path=<path>]\n"
        << "  agentos autodev generate-goal-docs job_id=<job_id>\n"
        << "  agentos autodev validate-spec job_id=<job_id>\n"
        << "  agentos autodev approve-spec job_id=<job_id> spec_hash=<sha256> [spec_revision=rev-001]\n"
        << "  agentos autodev tasks job_id=<job_id>\n"
        << "  agentos autodev turns job_id=<job_id>\n"
        << "  agentos autodev verify-task job_id=<job_id> task_id=<task_id> [related_turn_id=<turn_id>]\n"
        << "  agentos autodev verifications job_id=<job_id>\n"
        << "  agentos autodev diff-guard job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev diffs job_id=<job_id>\n"
        << "  agentos autodev acceptance-gate job_id=<job_id> task_id=<task_id>\n"
        << "  agentos autodev acceptances job_id=<job_id>\n"
        << "  agentos autodev final-review job_id=<job_id>\n"
        << "  agentos autodev final-reviews job_id=<job_id>\n"
        << "  agentos autodev events job_id=<job_id>\n"
        << "  agentos autodev execute-next-task job_id=<job_id>\n";
}

bool ParseBool(const std::string& value) {
    return value == "true" || value == "1" || value == "yes";
}

int RunSubmit(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    AutoDevSubmitRequest request;
    request.agentos_workspace = workspace;
    if (const auto it = options.find("target_repo_path"); it != options.end()) {
        request.target_repo_path = it->second;
    }
    if (const auto it = options.find("objective"); it != options.end()) {
        request.objective = it->second;
    }
    if (const auto it = options.find("skill_pack_path"); it != options.end()) {
        request.skill_pack_path = std::filesystem::path(it->second);
    }
    if (const auto it = options.find("isolation_mode"); it != options.end()) {
        request.isolation_mode = it->second;
    }
    if (const auto it = options.find("allow_dirty_target"); it != options.end()) {
        request.allow_dirty_target = ParseBool(it->second);
    }

    AutoDevStateStore store(workspace);
    const auto result = store.submit(request);
    if (!result.success) {
        std::cerr << "autodev submit failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev job submitted\n"
              << "job_id:             " << result.job.job_id << '\n'
              << "status:             " << result.job.status << '\n'
              << "phase:              " << result.job.phase << '\n'
              << "current_activity:   " << result.job.current_activity << '\n'
              << "target_repo_path:   " << result.job.target_repo_path.string() << '\n'
              << "job_worktree_path:  " << result.job.job_worktree_path.string() << " (planned)\n"
              << "isolation_mode:     " << result.job.isolation_mode << '\n'
              << "isolation_status:   " << result.job.isolation_status << '\n'
              << "allow_dirty_target: " << (result.job.allow_dirty_target ? "true" : "false") << '\n'
              << "next_action:        " << result.job.next_action << '\n'
              << "skill_pack_status:  " << result.job.skill_pack.status << '\n'
              << "job_dir:            " << result.job_dir.string() << '\n'
              << "job_json:           " << store.job_json_path(result.job.job_id).string() << '\n'
              << "events:             " << store.events_path(result.job.job_id).string() << '\n'
              << "\nWorkspace is not ready yet. No target files have been modified.\n";
    return 0;
}

int RunPrepareWorkspace(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev prepare-workspace failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev prepare-workspace failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.prepare_workspace(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev prepare-workspace failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:           " << result.job.status << '\n'
                      << "phase:            " << result.job.phase << '\n'
                      << "isolation_status: " << result.job.isolation_status << '\n'
                      << "next_action:      " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev workspace prepared\n"
              << "job_id:                " << result.job.job_id << '\n'
              << "status:                " << result.job.status << '\n'
              << "phase:                 " << result.job.phase << '\n'
              << "isolation_status:      " << result.job.isolation_status << '\n'
              << "target_repo_path:      " << result.job.target_repo_path.string() << '\n'
              << "job_worktree_path:     " << result.job.job_worktree_path.string() << '\n'
              << "created_from_head_sha: " << result.job.created_from_head_sha.value_or("") << '\n'
              << "worktree_created_at:   " << result.job.worktree_created_at.value_or("") << '\n'
              << "next_action:           " << result.job.next_action << '\n'
              << "\nTarget working tree files were not modified; work will continue in the job worktree.\n";
    return 0;
}

int RunLoadSkillPack(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev load-skill-pack failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev load-skill-pack failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    std::optional<std::filesystem::path> skill_pack_path;
    if (const auto it = options.find("skill_pack_path"); it != options.end() && !it->second.empty()) {
        skill_pack_path = std::filesystem::path(it->second);
    }

    AutoDevStateStore store(workspace);
    const auto result = store.load_skill_pack(job_id_it->second, skill_pack_path);
    if (!result.success) {
        std::cerr << "autodev load-skill-pack failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:            " << result.job.status << '\n'
                      << "skill_pack_status: " << result.job.skill_pack.status << '\n'
                      << "next_action:       " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev skill pack loaded\n"
              << "job_id:             " << result.job.job_id << '\n'
              << "status:             " << result.job.status << '\n'
              << "phase:              " << result.job.phase << '\n'
              << "skill_pack_status:  " << result.job.skill_pack.status << '\n'
              << "skill_pack_path:    "
              << (result.job.skill_pack.local_path.has_value() ? result.job.skill_pack.local_path->string() : "")
              << '\n'
              << "manifest_hash:      " << result.job.skill_pack.manifest_hash.value_or("") << '\n'
              << "snapshot:           " << result.snapshot_path.string() << '\n'
              << "next_action:        " << result.job.next_action << '\n'
              << "\nNo docs/goal files were generated and no Codex execution was started.\n";
    return 0;
}

int RunGenerateGoalDocs(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev generate-goal-docs failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev generate-goal-docs failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.generate_goal_docs(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev generate-goal-docs failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev goal docs generated\n"
              << "job_id:        " << result.job.job_id << '\n'
              << "status:        " << result.job.status << '\n'
              << "phase:         " << result.job.phase << '\n'
              << "goal_dir:      " << result.goal_dir.string() << '\n'
              << "files_written: " << result.written_files.size() << '\n'
              << "next_action:   " << result.job.next_action << '\n'
              << "\nThese are candidate working documents only. No spec was frozen and no Codex execution was started.\n";
    return 0;
}

int RunValidateSpec(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev validate-spec failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev validate-spec failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.validate_spec(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev validate-spec failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev spec validated\n"
              << "job_id:        " << result.job.job_id << '\n'
              << "status:        " << result.job.status << '\n'
              << "phase:         " << result.job.phase << '\n'
              << "approval_gate: " << result.job.approval_gate << '\n'
              << "spec_revision: " << result.spec_revision << '\n'
              << "spec_hash:     " << result.spec_hash << '\n'
              << "normalized:    " << result.normalized_path.string() << '\n'
              << "hash_file:     " << result.hash_path.string() << '\n'
              << "status_file:   " << result.status_path.string() << '\n'
              << "next_action:   " << result.job.next_action << '\n'
              << "\nThe spec snapshot is pending approval. No spec was frozen and no Codex execution was started.\n";
    return 0;
}

int RunApproveSpec(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev approve-spec failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev approve-spec failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto hash_it = options.find("spec_hash");
    if (hash_it == options.end() || hash_it->second.empty()) {
        std::cerr << "autodev approve-spec failed: spec_hash is required\n";
        return 1;
    }
    std::optional<std::string> spec_revision;
    if (const auto it = options.find("spec_revision"); it != options.end() && !it->second.empty()) {
        spec_revision = it->second;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.approve_spec(job_id_it->second, hash_it->second, spec_revision);
    if (!result.success) {
        std::cerr << "autodev approve-spec failed: " << result.error_message << '\n';
        if (!result.job.job_id.empty()) {
            std::cerr << "status:      " << result.job.status << '\n'
                      << "phase:       " << result.job.phase << '\n'
                      << "next_action: " << result.job.next_action << '\n';
        }
        return 1;
    }

    std::cout << "AutoDev spec approved\n"
              << "job_id:        " << result.job.job_id << '\n'
              << "status:        " << result.job.status << '\n'
              << "phase:         " << result.job.phase << '\n'
              << "approval_gate: " << result.job.approval_gate << '\n'
              << "spec_revision: " << result.spec_revision << '\n'
              << "spec_hash:     " << result.spec_hash << '\n'
              << "status_file:   " << result.status_path.string() << '\n'
              << "tasks_file:    " << result.tasks_path.string() << '\n'
              << "next_action:   " << result.job.next_action << '\n'
              << "\nThe frozen spec is approved. Codex execution was not started by this command.\n";
    return 0;
}

int RunTasks(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev tasks failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev tasks failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto tasks = store.load_tasks(job_id_it->second, &error);
    if (!tasks.has_value()) {
        std::cerr << "autodev tasks failed: " << error << '\n';
        return 1;
    }

    std::size_t passed = 0;
    for (const auto& task : *tasks) {
        if (task.status == "passed") {
            ++passed;
        }
    }

    std::cout << "AutoDev tasks\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << tasks->size() << '\n'
              << "passed: " << passed << '\n';
    for (const auto& task : *tasks) {
        std::cout << '\n'
                  << "- task_id:          " << task.task_id << '\n'
                  << "  title:            " << task.title << '\n'
                  << "  status:           " << task.status << '\n'
                  << "  current_activity: " << task.current_activity << '\n'
                  << "  spec_revision:    " << task.spec_revision << '\n'
                  << "  acceptance:       " << task.acceptance_passed << "/" << task.acceptance_total << '\n';
        if (task.verify_command.has_value()) {
            std::cout << "  verify_command:   " << *task.verify_command << '\n';
        }
        if (!task.allowed_files.empty()) {
            std::cout << "  allowed_files:    ";
            for (std::size_t i = 0; i < task.allowed_files.size(); ++i) {
                if (i != 0) {
                    std::cout << ", ";
                }
                std::cout << task.allowed_files[i];
            }
            std::cout << '\n';
        }
        if (!task.blocked_files.empty()) {
            std::cout << "  blocked_files:    ";
            for (std::size_t i = 0; i < task.blocked_files.size(); ++i) {
                if (i != 0) {
                    std::cout << ", ";
                }
                std::cout << task.blocked_files[i];
            }
            std::cout << '\n';
        }
    }
    return 0;
}

int RunEvents(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev events failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev events failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto lines = store.load_event_lines(job_id_it->second, &error);
    if (!lines.has_value()) {
        std::cerr << "autodev events failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev events\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << lines->size() << '\n';
    for (const auto& line : *lines) {
        try {
            const auto event = nlohmann::json::parse(line);
            std::cout << "- " << event.value("at", "") << " "
                      << event.value("type", "unknown");
            if (event.contains("status") && event.at("status").is_string()) {
                std::cout << " status=" << event.at("status").get<std::string>();
            }
            if (event.contains("phase") && event.at("phase").is_string()) {
                std::cout << " phase=" << event.at("phase").get<std::string>();
            }
            if (event.contains("next_action") && event.at("next_action").is_string()) {
                std::cout << " next_action=" << event.at("next_action").get<std::string>();
            }
            if (event.contains("blocker") && event.at("blocker").is_string() && !event.at("blocker").get<std::string>().empty()) {
                std::cout << " blocker=" << event.at("blocker").get<std::string>();
            }
            std::cout << '\n';
        } catch (...) {
            std::cout << "- " << line << '\n';
        }
    }
    return 0;
}

int RunTurns(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev turns failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev turns failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto turns = store.load_turns(job_id_it->second, &error);
    if (!turns.has_value()) {
        std::cerr << "autodev turns failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev turns\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << turns->size() << '\n';
    for (const auto& turn : *turns) {
        std::cout << '\n'
                  << "- turn_id:           " << turn.turn_id << '\n'
                  << "  task_id:           " << turn.task_id << '\n'
                  << "  status:            " << turn.status << '\n'
                  << "  adapter_kind:      " << turn.adapter_kind << '\n'
                  << "  continuity_mode:   " << turn.continuity_mode << '\n'
                  << "  event_stream_mode: " << turn.event_stream_mode << '\n'
                  << "  session_id:        " << turn.session_id << '\n'
                  << "  started_at:        " << turn.started_at << '\n';
        if (turn.completed_at.has_value()) {
            std::cout << "  completed_at:      " << *turn.completed_at << '\n';
        }
        if (turn.error_code.has_value()) {
            std::cout << "  error_code:        " << *turn.error_code << '\n';
        }
        if (turn.error_message.has_value()) {
            std::cout << "  error_message:     " << *turn.error_message << '\n';
        }
        if (turn.prompt_artifact.has_value()) {
            std::cout << "  prompt_artifact:   " << turn.prompt_artifact->string() << '\n';
        }
        if (turn.response_artifact.has_value()) {
            std::cout << "  response_artifact: " << turn.response_artifact->string() << '\n';
        }
    }
    return 0;
}

int RunVerifyTask(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev verify-task failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev verify-task failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto task_id_it = options.find("task_id");
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev verify-task failed: task_id is required\n";
        return 1;
    }
    std::optional<std::string> related_turn_id;
    if (const auto it = options.find("related_turn_id"); it != options.end() && !it->second.empty()) {
        related_turn_id = it->second;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.verify_task(job_id_it->second, task_id_it->second, related_turn_id);
    if (!result.success) {
        std::cerr << "autodev verify-task failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev task verified\n"
              << "job_id:          " << result.verification.job_id << '\n'
              << "task_id:         " << result.verification.task_id << '\n'
              << "verification_id: " << result.verification.verification_id << '\n'
              << "passed:          " << (result.verification.passed ? "true" : "false") << '\n'
              << "exit_code:       " << result.verification.exit_code << '\n'
              << "duration_ms:     " << result.verification.duration_ms << '\n'
              << "command:         " << result.verification.command << '\n'
              << "cwd:             " << result.verification.cwd.string() << '\n'
              << "verification:    " << result.verification_path.string() << '\n';
    if (result.verification.output_log_path.has_value()) {
        std::cout << "output_log:      " << result.verification.output_log_path->string() << '\n';
    }
    std::cout << "verify_report:   " << result.verify_report_path.string() << '\n';
    std::cout << "\nVerification facts were recorded. AcceptanceGate was not run and task status was not changed.\n";
    return result.verification.passed ? 0 : 1;
}

int RunVerifications(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev verifications failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev verifications failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto verifications = store.load_verifications(job_id_it->second, &error);
    if (!verifications.has_value()) {
        std::cerr << "autodev verifications failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev verifications\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << verifications->size() << '\n';
    for (const auto& verification : *verifications) {
        std::cout << '\n'
                  << "- verification_id: " << verification.verification_id << '\n'
                  << "  task_id:         " << verification.task_id << '\n'
                  << "  passed:          " << (verification.passed ? "true" : "false") << '\n'
                  << "  exit_code:       " << verification.exit_code << '\n'
                  << "  duration_ms:     " << verification.duration_ms << '\n'
                  << "  command:         " << verification.command << '\n'
                  << "  cwd:             " << verification.cwd.string() << '\n';
        if (verification.output_log_path.has_value()) {
            std::cout << "  output_log:      " << verification.output_log_path->string() << '\n';
        }
        if (verification.related_turn_id.has_value()) {
            std::cout << "  related_turn_id: " << *verification.related_turn_id << '\n';
        }
    }
    return 0;
}

void PrintStringList(const std::string& label, const std::vector<std::string>& values) {
    std::cout << label;
    if (values.empty()) {
        std::cout << " none\n";
        return;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            std::cout << ",";
        }
        std::cout << " " << values[i];
    }
    std::cout << '\n';
}

int RunDiffGuard(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev diff-guard failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev diff-guard failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto task_id_it = options.find("task_id");
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev diff-guard failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.diff_guard(job_id_it->second, task_id_it->second);
    if (!result.success) {
        std::cerr << "autodev diff-guard failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev diff guard checked\n"
              << "job_id:  " << result.diff_guard.job_id << '\n'
              << "task_id: " << result.diff_guard.task_id << '\n'
              << "diff_id: " << result.diff_guard.diff_id << '\n'
              << "passed:  " << (result.diff_guard.passed ? "true" : "false") << '\n'
              << "diffs:   " << result.diffs_path.string() << '\n';
    PrintStringList("changed_files:           ", result.diff_guard.changed_files);
    PrintStringList("blocked_file_violations:", result.diff_guard.blocked_file_violations);
    PrintStringList("outside_allowed_files:  ", result.diff_guard.outside_allowed_files);
    std::cout << "\nDiffGuard facts were recorded. AcceptanceGate was not run and task status was not changed.\n";
    return result.diff_guard.passed ? 0 : 1;
}

int RunDiffs(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev diffs failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev diffs failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto diffs = store.load_diffs(job_id_it->second, &error);
    if (!diffs.has_value()) {
        std::cerr << "autodev diffs failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev diffs\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << diffs->size() << '\n';
    for (const auto& diff : *diffs) {
        std::cout << '\n'
                  << "- diff_id: " << diff.diff_id << '\n'
                  << "  task_id: " << diff.task_id << '\n'
                  << "  passed:  " << (diff.passed ? "true" : "false") << '\n';
        PrintStringList("  changed_files:           ", diff.changed_files);
        PrintStringList("  blocked_file_violations:", diff.blocked_file_violations);
        PrintStringList("  outside_allowed_files:  ", diff.outside_allowed_files);
    }
    return 0;
}

int RunAcceptanceGate(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev acceptance-gate failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev acceptance-gate failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }
    const auto task_id_it = options.find("task_id");
    if (task_id_it == options.end() || task_id_it->second.empty()) {
        std::cerr << "autodev acceptance-gate failed: task_id is required\n";
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.acceptance_gate(job_id_it->second, task_id_it->second);
    if (!result.success) {
        std::cerr << "autodev acceptance-gate failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev acceptance gate checked\n"
              << "job_id:        " << result.acceptance.job_id << '\n'
              << "task_id:       " << result.acceptance.task_id << '\n'
              << "acceptance_id: " << result.acceptance.acceptance_id << '\n'
              << "passed:        " << (result.acceptance.passed ? "true" : "false") << '\n'
              << "task_status:   " << result.task.status << '\n'
              << "verification:  " << result.acceptance.verification_id.value_or("(none)") << '\n'
              << "diff:          " << result.acceptance.diff_id.value_or("(none)") << '\n'
              << "acceptance:    " << result.acceptance_path.string() << '\n';
    PrintStringList("reasons:       ", result.acceptance.reasons);
    std::cout << "\nAcceptanceGate updated only the task when runtime facts passed. Job completion was not evaluated.\n";
    return result.acceptance.passed ? 0 : 1;
}

int RunAcceptances(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev acceptances failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev acceptances failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto acceptances = store.load_acceptances(job_id_it->second, &error);
    if (!acceptances.has_value()) {
        std::cerr << "autodev acceptances failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev acceptances\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << acceptances->size() << '\n';
    for (const auto& acceptance : *acceptances) {
        std::cout << '\n'
                  << "- acceptance_id: " << acceptance.acceptance_id << '\n'
                  << "  task_id:       " << acceptance.task_id << '\n'
                  << "  passed:        " << (acceptance.passed ? "true" : "false") << '\n'
                  << "  verification:  " << acceptance.verification_id.value_or("(none)") << '\n'
                  << "  diff:          " << acceptance.diff_id.value_or("(none)") << '\n'
                  << "  checked_at:    " << acceptance.checked_at << '\n';
        PrintStringList("  reasons:       ", acceptance.reasons);
    }
    return 0;
}

int RunFinalReview(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev final-review failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev final-review failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    const auto result = store.final_review(job_id_it->second);
    if (!result.success) {
        std::cerr << "autodev final-review failed: " << result.error_message << '\n';
        return 1;
    }

    std::cout << "AutoDev final review checked\n"
              << "job_id:          " << result.final_review.job_id << '\n'
              << "final_review_id: " << result.final_review.final_review_id << '\n'
              << "passed:          " << (result.final_review.passed ? "true" : "false") << '\n'
              << "job_status:      " << result.job.status << '\n'
              << "job_phase:       " << result.job.phase << '\n'
              << "tasks:           " << result.final_review.tasks_passed << "/" << result.final_review.tasks_total << '\n'
              << "final_review:    " << result.final_review_path.string() << '\n'
              << "review_report:   " << result.final_review_report_path.string() << '\n';
    PrintStringList("changed_files:           ", result.final_review.changed_files);
    PrintStringList("blocked_file_violations:", result.final_review.blocked_file_violations);
    PrintStringList("outside_allowed_files:  ", result.final_review.outside_allowed_files);
    PrintStringList("reasons:                ", result.final_review.reasons);
    std::cout << "\nFinalReview records runtime facts and may advance the job to pr_ready. It never marks the job done.\n";
    return result.final_review.passed ? 0 : 1;
}

int RunFinalReviews(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev final-reviews failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev final-reviews failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto final_reviews = store.load_final_reviews(job_id_it->second, &error);
    if (!final_reviews.has_value()) {
        std::cerr << "autodev final-reviews failed: " << error << '\n';
        return 1;
    }

    std::cout << "AutoDev final reviews\n"
              << "job_id: " << job_id_it->second << '\n'
              << "total:  " << final_reviews->size() << '\n';
    for (const auto& final_review : *final_reviews) {
        std::cout << '\n'
                  << "- final_review_id: " << final_review.final_review_id << '\n'
                  << "  passed:          " << (final_review.passed ? "true" : "false") << '\n'
                  << "  tasks:           " << final_review.tasks_passed << "/" << final_review.tasks_total << '\n'
                  << "  checked_at:      " << final_review.checked_at << '\n';
        PrintStringList("  changed_files:           ", final_review.changed_files);
        PrintStringList("  blocked_file_violations:", final_review.blocked_file_violations);
        PrintStringList("  outside_allowed_files:  ", final_review.outside_allowed_files);
        PrintStringList("  reasons:                ", final_review.reasons);
    }
    return 0;
}

int RunSummary(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev summary failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev summary failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << "autodev summary failed: " << error << '\n';
        return 1;
    }
    const auto tasks = store.load_tasks(job_id_it->second, &error);
    if (!tasks.has_value()) {
        std::cerr << "autodev summary failed: " << error << '\n';
        return 1;
    }

    const auto verifications = store.load_verifications(job_id_it->second, nullptr).value_or(std::vector<AutoDevVerification>{});
    const auto diffs = store.load_diffs(job_id_it->second, nullptr).value_or(std::vector<AutoDevDiffGuard>{});
    const auto acceptances = store.load_acceptances(job_id_it->second, nullptr).value_or(std::vector<AutoDevAcceptanceGate>{});
    const auto final_reviews = store.load_final_reviews(job_id_it->second, nullptr).value_or(std::vector<AutoDevFinalReview>{});

    const auto latest_verification = [&verifications](const std::string& task_id) -> std::optional<AutoDevVerification> {
        for (auto it = verifications.rbegin(); it != verifications.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };
    const auto latest_diff = [&diffs](const std::string& task_id) -> std::optional<AutoDevDiffGuard> {
        for (auto it = diffs.rbegin(); it != diffs.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };
    const auto latest_acceptance = [&acceptances](const std::string& task_id) -> std::optional<AutoDevAcceptanceGate> {
        for (auto it = acceptances.rbegin(); it != acceptances.rend(); ++it) {
            if (it->task_id == task_id) {
                return *it;
            }
        }
        return std::nullopt;
    };

    std::size_t passed_tasks = 0;
    for (const auto& task : *tasks) {
        if (task.status == "passed") {
            ++passed_tasks;
        }
    }

    std::cout << "AutoDev summary\n"
              << "job_id:        " << job->job_id << '\n'
              << "status:        " << job->status << '\n'
              << "phase:         " << job->phase << '\n'
              << "next_action:   " << job->next_action << '\n'
              << "spec_revision: " << job->spec_revision.value_or("(none)") << '\n'
              << "tasks:         " << passed_tasks << "/" << tasks->size() << '\n'
              << "facts:         verifications=" << verifications.size()
              << " diffs=" << diffs.size()
              << " acceptances=" << acceptances.size()
              << " final_reviews=" << final_reviews.size() << '\n';

    std::cout << "\nTasks:\n";
    for (const auto& task : *tasks) {
        const auto verification = latest_verification(task.task_id);
        const auto diff = latest_diff(task.task_id);
        const auto acceptance = latest_acceptance(task.task_id);
        std::cout << "- " << task.task_id << " " << task.status << " - " << task.title << '\n'
                  << "  verification: "
                  << (verification.has_value() ? verification->verification_id : "(none)");
        if (verification.has_value()) {
            std::cout << " passed=" << (verification->passed ? "true" : "false");
        }
        std::cout << '\n'
                  << "  diff_guard:   "
                  << (diff.has_value() ? diff->diff_id : "(none)");
        if (diff.has_value()) {
            std::cout << " passed=" << (diff->passed ? "true" : "false");
        }
        std::cout << '\n'
                  << "  acceptance:   "
                  << (acceptance.has_value() ? acceptance->acceptance_id : "(none)");
        if (acceptance.has_value()) {
            std::cout << " passed=" << (acceptance->passed ? "true" : "false");
        }
        std::cout << '\n';
        if (diff.has_value() && (!diff->blocked_file_violations.empty() || !diff->outside_allowed_files.empty())) {
            PrintStringList("  diff_blocked_file_violations:", diff->blocked_file_violations);
            PrintStringList("  diff_outside_allowed_files:  ", diff->outside_allowed_files);
        }
        if (acceptance.has_value() && !acceptance->reasons.empty()) {
            PrintStringList("  acceptance_reasons:", acceptance->reasons);
        }
    }

    std::cout << "\nFinal review:\n";
    if (final_reviews.empty()) {
        std::cout << "  final_review_id: (none)\n";
    } else {
        const auto& final_review = final_reviews.back();
        std::cout << "  final_review_id: " << final_review.final_review_id << '\n'
                  << "  passed:          " << (final_review.passed ? "true" : "false") << '\n'
                  << "  tasks:           " << final_review.tasks_passed << "/" << final_review.tasks_total << '\n';
        PrintStringList("  reasons:         ", final_review.reasons);
    }
    return 0;
}

int RunExecuteNextTask(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev execute-next-task failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev execute-next-task failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << "autodev execute-next-task failed: " << error << '\n';
        return 1;
    }
    if (job->status != "running" || job->phase != "codex_execution") {
        std::cerr << "autodev execute-next-task failed: job is not in codex_execution\n"
                  << "status: " << job->status << '\n'
                  << "phase:  " << job->phase << '\n';
        return 1;
    }
    if (job->approval_gate != "none") {
        std::cerr << "autodev execute-next-task failed: approval gate is still active: " << job->approval_gate << '\n';
        return 1;
    }
    if (job->isolation_status != "ready" || !std::filesystem::exists(job->job_worktree_path)) {
        std::cerr << "autodev execute-next-task failed: workspace isolation is not ready\n"
                  << "isolation_status: " << job->isolation_status << '\n'
                  << "job_worktree_path: " << job->job_worktree_path.string() << '\n';
        return 1;
    }
    if (!job->spec_revision.has_value() || !job->spec_hash.has_value()) {
        std::cerr << "autodev execute-next-task failed: no approved frozen spec is recorded on the job\n";
        return 1;
    }

    const auto tasks = store.load_tasks(job_id_it->second, &error);
    if (!tasks.has_value()) {
        std::cerr << "autodev execute-next-task failed: " << error << '\n';
        return 1;
    }
    const auto pending_task = std::find_if(tasks->begin(), tasks->end(), [](const AutoDevTask& task) {
        return task.status == "pending";
    });
    if (pending_task == tasks->end()) {
        std::cerr << "autodev execute-next-task failed: no pending AutoDev task is available\n";
        return 1;
    }

    const CodexCliAutoDevAdapter adapter;
    const auto profile = adapter.profile();
    constexpr const char* blocked_reason = "Codex CLI AutoDev execution is not implemented in this build";
    store.record_execution_blocked(*job, *pending_task, profile, blocked_reason);
    std::cout << "AutoDev execution preflight\n"
              << "job_id:             " << job->job_id << '\n'
              << "status:             " << job->status << '\n'
              << "phase:              " << job->phase << '\n'
              << "spec_revision:      " << *job->spec_revision << '\n'
              << "spec_hash:          " << *job->spec_hash << '\n'
              << "job_worktree_path:  " << job->job_worktree_path.string() << '\n'
              << '\n'
              << "Next task:\n"
              << "  task_id:          " << pending_task->task_id << '\n'
              << "  title:            " << pending_task->title << '\n'
              << "  status:           " << pending_task->status << '\n'
              << "  current_activity: " << pending_task->current_activity << '\n';
    if (pending_task->verify_command.has_value()) {
        std::cout << "  verify_command:   " << *pending_task->verify_command << '\n';
    }

    std::cout << '\n'
              << "Execution adapter:\n"
              << "  adapter_kind:                " << profile.adapter_kind << '\n'
              << "  adapter_name:                " << profile.adapter_name << '\n'
              << "  continuity_mode:             " << profile.continuity_mode << '\n'
              << "  event_stream_mode:           " << profile.event_stream_mode << '\n'
              << "  supports_persistent_session: " << (profile.supports_persistent_session ? "true" : "false") << '\n'
              << "  supports_native_event_stream:" << (profile.supports_native_event_stream ? " true" : " false") << '\n'
              << "  supports_same_thread_repair: " << (profile.supports_same_thread_repair ? "true" : "false") << '\n'
              << "  production_final_executor:   " << (profile.production_final_executor ? "true" : "false") << '\n'
              << "  healthy:                     " << (adapter.healthy() ? "true" : "false") << '\n'
              << "  risk_level:                  " << profile.risk_level << '\n'
              << '\n'
              << "Execution was not started. " << blocked_reason << ".\n"
              << "Task status and job completion remain controlled by AgentOS runtime facts.\n";
    return 1;
}

int RunStatus(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    const auto options = ParseOptionsFromArgs(argc, argv, 3);
    const auto job_id_it = options.find("job_id");
    if (job_id_it == options.end() || job_id_it->second.empty()) {
        std::cerr << "autodev status failed: job_id is required\n";
        return 1;
    }
    if (!IsValidAutoDevJobId(job_id_it->second)) {
        std::cerr << "autodev status failed: invalid job_id: " << job_id_it->second << '\n';
        return 1;
    }

    AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job(job_id_it->second, &error);
    if (!job.has_value()) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "Job: " << job->job_id << '\n'
              << "Status: " << job->status << '\n'
              << "Phase: " << job->phase << '\n'
              << "Current activity: " << job->current_activity << '\n'
              << "Approval gate: " << job->approval_gate << '\n'
              << '\n'
              << "Paths:\n"
              << "  agentos_workspace: " << job->agentos_workspace.string() << '\n'
              << "  target_repo_path:  " << job->target_repo_path.string() << '\n'
              << "  job_worktree_path: " << job->job_worktree_path.string()
              << (job->isolation_status == "ready" ? "" : " (planned)") << '\n'
              << '\n'
              << "Isolation:\n"
              << "  mode:              " << job->isolation_mode << '\n'
              << "  status:            " << job->isolation_status << '\n'
              << "  allow_dirty_target:" << (job->allow_dirty_target ? " true" : " false") << '\n'
              << "  head_sha:          " << job->created_from_head_sha.value_or("(none)") << '\n'
              << "  worktree_created:  " << job->worktree_created_at.value_or("(none)") << '\n'
              << '\n'
              << "Skill Pack:\n"
              << "  status: " << job->skill_pack.status << '\n';
    if (job->skill_pack.local_path.has_value()) {
        std::cout << "  path:   " << job->skill_pack.local_path->string() << '\n';
    }
    if (job->skill_pack.manifest_hash.has_value()) {
        std::cout << "  hash:   " << *job->skill_pack.manifest_hash << '\n';
    }
    if (job->skill_pack.error.has_value()) {
        std::cout << "  error:  " << *job->skill_pack.error << '\n';
    }
    if (job->spec_revision.has_value()) {
        std::cout << '\n'
                  << "Spec:\n"
                  << "  schema_version: " << job->schema_version.value_or("(none)") << '\n'
                  << "  revision:       " << *job->spec_revision << '\n'
                  << "  hash:           " << job->spec_hash.value_or("(none)") << '\n';
    }
    if (std::filesystem::exists(store.tasks_path(job->job_id))) {
        try {
            std::ifstream input(store.tasks_path(job->job_id), std::ios::binary);
            nlohmann::json tasks;
            input >> tasks;
            const auto total = tasks.is_array() ? tasks.size() : 0U;
            std::size_t passed = 0;
            if (tasks.is_array()) {
                for (const auto& task : tasks) {
                    if (task.value("status", "") == "passed") {
                        ++passed;
                    }
                }
            }
            std::cout << '\n'
                      << "Tasks:\n"
                      << "  total:  " << total << '\n'
                      << "  passed: " << passed << '\n';
        } catch (...) {
            std::cout << '\n'
                      << "Tasks:\n"
                      << "  error: failed to read tasks.json\n";
        }
    }
    std::cout << '\n'
              << "Next action:\n";
    if (job->next_action == "none") {
        std::cout << "  none";
    } else {
        std::cout << "  agentos autodev "
                  << (job->next_action == "prepare_workspace" ? "prepare-workspace" : job->next_action)
                  << " job_id=" << job->job_id;
        if (job->next_action == "approve_spec" && job->spec_hash.has_value()) {
            std::cout << " spec_hash=" << *job->spec_hash;
        }
    }
    std::cout << '\n'
              << '\n';
    if (job->isolation_status == "ready") {
        std::cout << "Workspace is ready. Future AutoDev writes must use the job worktree.\n";
    } else {
        std::cout << "Workspace is not ready yet. No target files have been modified.\n";
    }
    return 0;
}

}  // namespace

int RunAutoDevCommand(const std::filesystem::path& workspace, const int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }
    const std::string subcommand = argv[2];
    if (subcommand == "submit") {
        return RunSubmit(workspace, argc, argv);
    }
    if (subcommand == "status") {
        return RunStatus(workspace, argc, argv);
    }
    if (subcommand == "summary") {
        return RunSummary(workspace, argc, argv);
    }
    if (subcommand == "prepare-workspace" || subcommand == "prepare_workspace") {
        return RunPrepareWorkspace(workspace, argc, argv);
    }
    if (subcommand == "load-skill-pack" || subcommand == "load_skill_pack") {
        return RunLoadSkillPack(workspace, argc, argv);
    }
    if (subcommand == "generate-goal-docs" || subcommand == "generate_goal_docs") {
        return RunGenerateGoalDocs(workspace, argc, argv);
    }
    if (subcommand == "validate-spec" || subcommand == "validate_spec") {
        return RunValidateSpec(workspace, argc, argv);
    }
    if (subcommand == "approve-spec" || subcommand == "approve_spec") {
        return RunApproveSpec(workspace, argc, argv);
    }
    if (subcommand == "tasks") {
        return RunTasks(workspace, argc, argv);
    }
    if (subcommand == "turns") {
        return RunTurns(workspace, argc, argv);
    }
    if (subcommand == "verify-task" || subcommand == "verify_task") {
        return RunVerifyTask(workspace, argc, argv);
    }
    if (subcommand == "verifications") {
        return RunVerifications(workspace, argc, argv);
    }
    if (subcommand == "diff-guard" || subcommand == "diff_guard") {
        return RunDiffGuard(workspace, argc, argv);
    }
    if (subcommand == "diffs") {
        return RunDiffs(workspace, argc, argv);
    }
    if (subcommand == "acceptance-gate" || subcommand == "acceptance_gate") {
        return RunAcceptanceGate(workspace, argc, argv);
    }
    if (subcommand == "acceptances") {
        return RunAcceptances(workspace, argc, argv);
    }
    if (subcommand == "final-review" || subcommand == "final_review") {
        return RunFinalReview(workspace, argc, argv);
    }
    if (subcommand == "final-reviews" || subcommand == "final_reviews") {
        return RunFinalReviews(workspace, argc, argv);
    }
    if (subcommand == "events") {
        return RunEvents(workspace, argc, argv);
    }
    if (subcommand == "execute-next-task" || subcommand == "execute_next_task") {
        return RunExecuteNextTask(workspace, argc, argv);
    }

    std::cerr << "Unknown autodev subcommand: " << subcommand << '\n';
    PrintUsage();
    return 1;
}

}  // namespace agentos
