#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_state_store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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

}  // namespace

int main() {
    TestJobIdValidation();
    TestSubmitCreatesRuntimeFactsOnly();
    TestSubmitWithoutSkillPackStaysNotLoaded();
    TestSubmitMissingTargetFailsWithoutJob();
    TestLoadRejectsInvalidJobId();

    if (failures != 0) {
        std::cerr << failures << " AutoDev test assertion(s) failed\n";
        return 1;
    }
    std::cout << "AutoDev tests passed\n";
    return 0;
}
