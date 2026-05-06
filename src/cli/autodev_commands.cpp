#include "cli/autodev_commands.hpp"

#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_state_store.hpp"

#include <iostream>
#include <map>
#include <string>

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
        << "  agentos autodev prepare-workspace job_id=<job_id>\n"
        << "  agentos autodev load-skill-pack job_id=<job_id> [skill_pack_path=<path>]\n";
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
              << "\nTarget repo was not modified; work will continue in the job worktree.\n";
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
    std::cout << '\n'
              << "Next action:\n"
              << "  agentos autodev "
              << (job->next_action == "prepare_workspace" ? "prepare-workspace" : job->next_action)
              << " job_id=" << job->job_id << '\n'
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
    if (subcommand == "prepare-workspace" || subcommand == "prepare_workspace") {
        return RunPrepareWorkspace(workspace, argc, argv);
    }
    if (subcommand == "load-skill-pack" || subcommand == "load_skill_pack") {
        return RunLoadSkillPack(workspace, argc, argv);
    }

    std::cerr << "Unknown autodev subcommand: " << subcommand << '\n';
    PrintUsage();
    return 1;
}

}  // namespace agentos
