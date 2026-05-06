#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_state_store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace(const std::string& name) {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_autodev_tests" / name;
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int RunShell(const std::string& command) {
    return std::system(command.c_str());
}

std::string QuoteShellArg(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

void InitGitRepo(const std::filesystem::path& repo) {
    std::filesystem::create_directories(repo);
    Expect(RunShell("git -C " + QuoteShellArg(repo.string()) + " init >/dev/null 2>&1") == 0,
        "test fixture should initialize git repo");
    Expect(RunShell("git -C " + QuoteShellArg(repo.string()) + " config user.email test@example.com") == 0,
        "test fixture should configure git email");
    Expect(RunShell("git -C " + QuoteShellArg(repo.string()) + " config user.name Test") == 0,
        "test fixture should configure git user");
    {
        std::ofstream readme(repo / "README.md", std::ios::binary);
        readme << "fixture\n";
    }
    Expect(RunShell("git -C " + QuoteShellArg(repo.string()) + " add README.md") == 0,
        "test fixture should stage initial file");
    Expect(RunShell("git -C " + QuoteShellArg(repo.string()) + " commit -m initial >/dev/null 2>&1") == 0,
        "test fixture should commit initial file");
}

void TestJobIdValidation() {
    const auto job_id = agentos::GenerateAutoDevJobId();
    Expect(agentos::IsValidAutoDevJobId(job_id), "generated AutoDev job id should match the stable regex");
    Expect(agentos::IsValidAutoDevJobId("autodev-20260506-164233-a1b2c3"),
        "valid AutoDev job id should pass validation");
    Expect(!agentos::IsValidAutoDevJobId("../autodev-20260506-164233-a1b2c3"),
        "path traversal should not pass job id validation");
    Expect(!agentos::IsValidAutoDevJobId("dev-1778050066404"),
        "legacy dev job id should not pass AutoDev validation");
}

void TestSubmitCreatesRuntimeFactsOnly() {
    const auto workspace = FreshWorkspace("submit_runtime_facts");
    const auto target = workspace / "target_app";
    std::filesystem::create_directories(target);
    const auto skill_pack = workspace / "skills";
    std::filesystem::create_directories(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto result = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Fix login 500",
        .skill_pack_path = skill_pack,
    });

    Expect(result.success, "submit should succeed for an existing target path");
    Expect(agentos::IsValidAutoDevJobId(result.job.job_id), "submit should allocate a valid AutoDev job id");
    Expect(std::filesystem::exists(store.job_json_path(result.job.job_id)),
        "submit should create job.json under AgentOS runtime");
    Expect(std::filesystem::exists(store.events_path(result.job.job_id)),
        "submit should create events.ndjson under AgentOS runtime");
    Expect(result.job.isolation_status == "pending", "submit should keep isolation pending");
    Expect(result.job.next_action == "prepare_workspace", "submit should set next_action to prepare_workspace");
    Expect(result.job.skill_pack.status == "declared", "skill_pack_path should be recorded as declared");
    Expect(result.job.skill_pack.local_path.has_value() &&
               *result.job.skill_pack.local_path == std::filesystem::absolute(skill_pack).lexically_normal(),
        "skill_pack_path should be normalized and recorded without loading");
    Expect(!std::filesystem::exists(result.job.job_worktree_path),
        "submit should not create planned job_worktree_path");
    Expect(!std::filesystem::exists(target / "runtime" / "autodev"),
        "submit should not write runtime facts into target repo");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "submit should not generate docs/goal in target repo");

    const auto job_json = nlohmann::json::parse(ReadFile(store.job_json_path(result.job.job_id)));
    Expect(job_json["status"] == "submitted", "job.json should record submitted status");
    Expect(job_json["phase"] == "workspace_preparing", "job.json should record workspace_preparing phase");
    Expect(job_json["current_activity"] == "none", "job.json should record no current activity");
    Expect(job_json["created_from_head_sha"].is_null(), "submit should not inspect git HEAD");
    Expect(job_json["worktree_created_at"].is_null(), "submit should not create worktree timestamp");

    const auto events = ReadFile(store.events_path(result.job.job_id));
    Expect(events.find("\"type\":\"autodev.job.submitted\"") != std::string::npos,
        "events.ndjson should record submit event");
    Expect(events.find("\"planned_worktree_path\"") != std::string::npos,
        "submit event should record planned worktree path");
}

void TestSubmitWithoutSkillPackStaysNotLoaded() {
    const auto workspace = FreshWorkspace("submit_no_skill_pack");
    const auto target = workspace / "target_app";
    std::filesystem::create_directories(target);

    agentos::AutoDevStateStore store(workspace);
    const auto result = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Create a small feature",
        .skill_pack_path = std::nullopt,
    });

    Expect(result.success, "submit without skill_pack_path should succeed");
    Expect(result.job.skill_pack.status == "not_loaded",
        "missing skill_pack_path should keep skill pack not_loaded");
    Expect(!result.job.skill_pack.local_path.has_value(),
        "missing skill_pack_path should not record a local path");
}

void TestSubmitMissingTargetFailsWithoutJob() {
    const auto workspace = FreshWorkspace("missing_target");
    const auto missing_target = workspace / "missing_target";

    agentos::AutoDevStateStore store(workspace);
    const auto result = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = missing_target,
        .objective = "Fix bug",
        .skill_pack_path = std::nullopt,
    });

    Expect(!result.success, "submit should fail when target_repo_path does not exist");
    Expect(!std::filesystem::exists(store.jobs_dir()),
        "failed submit should not create AutoDev jobs directory");
}

void TestLoadRejectsInvalidJobId() {
    const auto workspace = FreshWorkspace("invalid_status");
    agentos::AutoDevStateStore store(workspace);
    std::string error;
    const auto job = store.load_job("../bad", &error);
    Expect(!job.has_value(), "load_job should reject invalid job ids");
    Expect(error.find("invalid AutoDev job_id") != std::string::npos,
        "load_job should report invalid job id");
}

void TestPrepareWorkspaceCreatesGitWorktree() {
    const auto workspace = FreshWorkspace("prepare_workspace");
    const auto target = workspace / "target_app";
    InitGitRepo(target);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Prepare workspace",
        .skill_pack_path = std::nullopt,
    });
    Expect(submit.success, "submit before prepare_workspace should succeed");

    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace should succeed for clean git repo");
    Expect(prepared.job.isolation_status == "ready", "prepare_workspace should mark isolation ready");
    Expect(prepared.job.phase == "system_understanding", "prepare_workspace should advance phase");
    Expect(prepared.job.current_activity == "none", "prepare_workspace should clear current activity");
    Expect(prepared.job.created_from_head_sha.has_value() && !prepared.job.created_from_head_sha->empty(),
        "prepare_workspace should record source HEAD sha");
    Expect(prepared.job.worktree_created_at.has_value(), "prepare_workspace should record worktree creation time");
    Expect(std::filesystem::exists(prepared.job.job_worktree_path / ".git"),
        "prepare_workspace should create git worktree at job_worktree_path");
    Expect(!std::filesystem::exists(target / "runtime" / "autodev"),
        "prepare_workspace should not write runtime facts into target repo");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "prepare_workspace should not generate docs/goal in target repo");

    const auto job_json = nlohmann::json::parse(ReadFile(store.job_json_path(submit.job.job_id)));
    Expect(job_json["isolation_status"] == "ready", "job.json should persist ready isolation");
    Expect(job_json["phase"] == "system_understanding", "job.json should persist next phase");

    const auto events = ReadFile(store.events_path(submit.job.job_id));
    Expect(events.find("\"type\":\"autodev.workspace.prepared\"") != std::string::npos,
        "events.ndjson should record workspace prepared event");
}

void TestPrepareWorkspaceBlocksDirtyTarget() {
    const auto workspace = FreshWorkspace("prepare_dirty_target");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    {
        std::ofstream dirty(target / "dirty.txt", std::ios::binary);
        dirty << "dirty\n";
    }

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Prepare dirty workspace",
        .skill_pack_path = std::nullopt,
    });
    Expect(submit.success, "submit for dirty target should still succeed");

    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(!prepared.success, "prepare_workspace should block dirty target by default");
    Expect(prepared.job.status == "blocked", "dirty prepare should mark job blocked");
    Expect(prepared.job.isolation_status == "blocked", "dirty prepare should block isolation");
    Expect(prepared.job.blocker.has_value() &&
               prepared.job.blocker->find("uncommitted changes") != std::string::npos,
        "dirty prepare should explain dirty target blocker");
    Expect(!std::filesystem::exists(prepared.job.job_worktree_path),
        "dirty prepare should not create worktree");
}

}  // namespace

int main() {
    TestJobIdValidation();
    TestSubmitCreatesRuntimeFactsOnly();
    TestSubmitWithoutSkillPackStaysNotLoaded();
    TestSubmitMissingTargetFailsWithoutJob();
    TestLoadRejectsInvalidJobId();
    TestPrepareWorkspaceCreatesGitWorktree();
    TestPrepareWorkspaceBlocksDirtyTarget();

    if (failures != 0) {
        std::cerr << failures << " AutoDev test assertion(s) failed\n";
        return 1;
    }
    std::cout << "AutoDev tests passed\n";
    return 0;
}
