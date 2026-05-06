#include "autodev/autodev_execution_adapter.hpp"
#include "autodev/autodev_job_id.hpp"
#include "autodev/autodev_state_store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <cstdlib>
#include <string>
#include <vector>

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

void CreateAutoDevSkillPackFixture(const std::filesystem::path& root, const bool complete = true) {
    const std::vector<std::string> steps = {
        "00-understand-system",
        "01-grill-requirements",
        "02-spec-freeze",
        "03-impact-analysis",
        "04-task-slice",
        "05-goal-pack",
        "07-verify-loop",
        "08-goal-review",
    };
    const auto limit = complete ? steps.size() : steps.size() - 1;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto dir = root / steps[i];
        std::filesystem::create_directories(dir);
        std::ofstream skill(dir / "SKILL.md", std::ios::binary);
        skill << "---\nname: " << steps[i] << "\n---\n# " << steps[i] << "\n";
    }
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

void TestCodexCliExecutionAdapterProfileIsTransitional() {
    const auto profile = agentos::CodexCliAutoDevAdapterProfile();
    Expect(profile.adapter_kind == "codex_cli",
        "Codex CLI AutoDev profile should identify the adapter kind");
    Expect(profile.adapter_name == "CodexCliAutoDevAdapter",
        "Codex CLI AutoDev profile should identify the transitional adapter name");
    Expect(profile.continuity_mode == "best_effort_context",
        "Codex CLI AutoDev profile should be honest about best-effort continuity");
    Expect(profile.event_stream_mode == "synthetic",
        "Codex CLI AutoDev profile should be honest about synthetic events");
    Expect(!profile.supports_persistent_session,
        "Codex CLI AutoDev profile should not claim persistent sessions");
    Expect(!profile.supports_native_event_stream,
        "Codex CLI AutoDev profile should not claim native event streams");
    Expect(!profile.supports_same_thread_repair,
        "Codex CLI AutoDev profile should not claim same-thread repair");
    Expect(!profile.production_final_executor,
        "Codex CLI AutoDev profile should not claim final production executor status");
    const auto profile_json = agentos::ToJson(profile);
    Expect(profile_json["adapter_kind"] == "codex_cli",
        "Codex CLI AutoDev profile JSON should record adapter kind");
    Expect(profile_json["continuity_mode"] == "best_effort_context",
        "Codex CLI AutoDev profile JSON should record continuity mode");
    Expect(profile_json["event_stream_mode"] == "synthetic",
        "Codex CLI AutoDev profile JSON should record event stream mode");
    const agentos::CodexCliAutoDevAdapter adapter;
    Expect(adapter.profile().adapter_kind == "codex_cli",
        "Codex CLI AutoDev adapter should expose its profile through the adapter interface");
    Expect(!adapter.healthy(),
        "Codex CLI AutoDev adapter should fail closed until execution is implemented");
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

void TestLoadSkillPackRecordsSnapshotOnly() {
    const auto workspace = FreshWorkspace("load_skill_pack");
    const auto target = workspace / "target_app";
    std::filesystem::create_directories(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Load skills",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before load_skill_pack should succeed");

    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack should succeed for complete fixture");
    Expect(loaded.job.skill_pack.status == "loaded", "load_skill_pack should mark skill pack loaded");
    Expect(loaded.job.skill_pack.manifest_hash.has_value() && !loaded.job.skill_pack.manifest_hash->empty(),
        "load_skill_pack should record manifest hash");
    Expect(std::filesystem::exists(loaded.snapshot_path),
        "load_skill_pack should write skill pack snapshot to runtime artifacts");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "load_skill_pack should not generate docs/goal in target repo");
    Expect(!std::filesystem::exists(submit.job.job_worktree_path),
        "load_skill_pack should not create planned worktree");

    const auto snapshot = nlohmann::json::parse(ReadFile(loaded.snapshot_path));
    Expect(snapshot["required_steps"].size() == 8,
        "skill pack snapshot should record required AutoDev steps");
    Expect(snapshot["manifest_hash"] == *loaded.job.skill_pack.manifest_hash,
        "skill pack snapshot hash should match job binding hash");

    const auto events = ReadFile(store.events_path(submit.job.job_id));
    Expect(events.find("\"type\":\"autodev.skill_pack.loaded\"") != std::string::npos,
        "events.ndjson should record skill pack loaded event");
}

void TestLoadSkillPackBlocksMissingSteps() {
    const auto workspace = FreshWorkspace("load_skill_pack_missing_step");
    const auto target = workspace / "target_app";
    std::filesystem::create_directories(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack, false);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Load broken skills",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before broken load_skill_pack should succeed");

    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(!loaded.success, "load_skill_pack should fail when required steps are missing");
    Expect(loaded.job.status == "blocked", "failed skill pack load should block job");
    Expect(loaded.job.skill_pack.status == "validation_failed",
        "missing required step should mark validation_failed");
    Expect(loaded.job.blocker.has_value() &&
               loaded.job.blocker->find("missing required AutoDev step") != std::string::npos,
        "failed skill pack load should explain missing required step");
}

void TestGenerateGoalDocsWritesOnlyJobWorktree() {
    const auto workspace = FreshWorkspace("generate_goal_docs");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Generate goal docs",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before generate_goal_docs should succeed");
    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace before generate_goal_docs should succeed");
    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack before generate_goal_docs should succeed");

    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(generated.success, "generate_goal_docs should succeed after workspace and skill pack are ready");
    Expect(generated.job.phase == "requirements_grilling",
        "generate_goal_docs should advance to requirements_grilling");
    Expect(generated.job.next_action == "validate_spec",
        "generate_goal_docs should set validate_spec next action");
    Expect(generated.written_files.size() == 13,
        "generate_goal_docs should write the candidate goal doc skeleton set");
    Expect(std::filesystem::exists(prepared.job.job_worktree_path / "docs" / "goal" / "GOAL.md"),
        "generate_goal_docs should write GOAL.md under job worktree");
    Expect(std::filesystem::exists(prepared.job.job_worktree_path / "docs" / "goal" / "AUTODEV_SPEC.json"),
        "generate_goal_docs should write candidate AUTODEV_SPEC.json under job worktree");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "generate_goal_docs should not write docs/goal into target repo");

    const auto spec = nlohmann::json::parse(ReadFile(prepared.job.job_worktree_path / "docs" / "goal" / "AUTODEV_SPEC.json"));
    Expect(spec["status"] == "candidate_skeleton", "AUTODEV_SPEC skeleton should be candidate-only");
    Expect(spec["tasks"].empty(), "AUTODEV_SPEC skeleton should not define execution tasks");

    const auto events = ReadFile(store.events_path(submit.job.job_id));
    Expect(events.find("\"type\":\"autodev.goal_docs.generated\"") != std::string::npos,
        "events.ndjson should record goal docs generated event");
}

void TestGenerateGoalDocsBlocksUntilReady() {
    const auto workspace = FreshWorkspace("generate_goal_docs_blocked");
    const auto target = workspace / "target_app";
    std::filesystem::create_directories(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Generate too early",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before blocked generate_goal_docs should succeed");

    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(!generated.success, "generate_goal_docs should fail before workspace is ready");
    Expect(generated.job.status == "blocked", "generate_goal_docs before workspace should block job");
    Expect(generated.job.blocker.has_value() &&
               generated.job.blocker->find("workspace is not ready") != std::string::npos,
        "generate_goal_docs should explain workspace readiness blocker");
    Expect(!std::filesystem::exists(target / "docs" / "goal"),
        "blocked generate_goal_docs should not write target docs");
}

void TestValidateSpecSnapshotsPendingRevision() {
    const auto workspace = FreshWorkspace("validate_spec");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Validate spec",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before validate_spec should succeed");
    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace before validate_spec should succeed");
    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack before validate_spec should succeed");
    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(generated.success, "generate_goal_docs before validate_spec should succeed");

    const auto validated = store.validate_spec(submit.job.job_id);
    Expect(validated.success, "validate_spec should validate generated candidate AUTODEV_SPEC");
    Expect(validated.job.status == "awaiting_approval",
        "validate_spec should stop at approval instead of continuing");
    Expect(validated.job.phase == "goal_packing",
        "validate_spec should move to goal_packing after pending snapshot");
    Expect(validated.job.approval_gate == "before_code_execution",
        "validate_spec should set before_code_execution approval gate");
    Expect(validated.job.next_action == "approve_spec",
        "validate_spec should point at the approval action");
    Expect(validated.spec_revision == "rev-001",
        "validate_spec should create first spec revision");
    Expect(validated.spec_hash.size() == 64,
        "validate_spec should compute sha256 spec hash");
    Expect(std::filesystem::exists(validated.normalized_path),
        "validate_spec should write normalized spec snapshot");
    Expect(std::filesystem::exists(validated.hash_path),
        "validate_spec should write hash file");
    Expect(std::filesystem::exists(validated.status_path),
        "validate_spec should write revision status file");
    Expect(!std::filesystem::exists(target / "runtime" / "autodev"),
        "validate_spec should not write runtime facts into target repo");

    const auto revision_status = nlohmann::json::parse(ReadFile(validated.status_path));
    Expect(revision_status["status"] == "pending_approval",
        "spec revision status should be pending approval");
    Expect(revision_status["approval_gate"] == "before_code_execution",
        "spec revision status should record approval gate");

    const auto events = ReadFile(store.events_path(submit.job.job_id));
    Expect(events.find("\"type\":\"autodev.spec.validated\"") != std::string::npos,
        "events.ndjson should record spec validated event");
}

void TestValidateSpecBlocksUnsupportedSchema() {
    const auto workspace = FreshWorkspace("validate_spec_bad_schema");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Validate bad spec",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before unsupported validate_spec should succeed");
    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace before unsupported validate_spec should succeed");
    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack before unsupported validate_spec should succeed");
    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(generated.success, "generate_goal_docs before unsupported validate_spec should succeed");

    const auto spec_path = prepared.job.job_worktree_path / "docs" / "goal" / "AUTODEV_SPEC.json";
    auto spec = nlohmann::json::parse(ReadFile(spec_path));
    spec["schema_version"] = "9.9.9";
    {
        std::ofstream output(spec_path, std::ios::binary | std::ios::trunc);
        output << spec.dump(2) << '\n';
    }

    const auto validated = store.validate_spec(submit.job.job_id);
    Expect(!validated.success, "validate_spec should block unsupported schema versions");
    Expect(validated.job.status == "blocked",
        "unsupported schema should block the job");
    Expect(validated.job.blocker.has_value() &&
               validated.job.blocker->find("unsupported AUTODEV_SPEC schema_version") != std::string::npos,
        "unsupported schema should explain blocker");
}

void TestApproveSpecRequiresNonEmptyTasks() {
    const auto workspace = FreshWorkspace("approve_spec_empty_tasks");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Approve empty spec",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before empty approve_spec should succeed");
    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace before empty approve_spec should succeed");
    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack before empty approve_spec should succeed");
    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(generated.success, "generate_goal_docs before empty approve_spec should succeed");
    const auto validated = store.validate_spec(submit.job.job_id);
    Expect(validated.success, "validate_spec before empty approve_spec should succeed");

    const auto approved = store.approve_spec(submit.job.job_id, validated.spec_hash);
    Expect(!approved.success, "approve_spec should block empty generated task list");
    Expect(approved.job.status == "blocked",
        "empty task approval should block job");
    Expect(approved.job.blocker.has_value() &&
               approved.job.blocker->find("tasks must not be empty") != std::string::npos,
        "empty task approval should explain blocker");
}

void TestApproveSpecFreezesHashBoundRevision() {
    const auto workspace = FreshWorkspace("approve_spec");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Approve spec",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before approve_spec should succeed");
    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace before approve_spec should succeed");
    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack before approve_spec should succeed");
    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(generated.success, "generate_goal_docs before approve_spec should succeed");

    const auto spec_path = prepared.job.job_worktree_path / "docs" / "goal" / "AUTODEV_SPEC.json";
    auto spec = nlohmann::json::parse(ReadFile(spec_path));
    spec["tasks"] = nlohmann::json::array({
        {
            {"task_id", "task-001"},
            {"title", "Implement approved fixture task"},
            {"allowed_files", nlohmann::json::array({"README.md"})},
            {"blocked_files", nlohmann::json::array()},
            {"verify_command", "true"},
            {"acceptance", nlohmann::json::array({"README.md remains readable"})},
        }
    });
    {
        std::ofstream output(spec_path, std::ios::binary | std::ios::trunc);
        output << spec.dump(2) << '\n';
    }

    const auto validated = store.validate_spec(submit.job.job_id);
    Expect(validated.success, "validate_spec before approve_spec should succeed for nonempty tasks");
    const auto wrong_hash = store.approve_spec(submit.job.job_id, std::string(64, '0'));
    Expect(!wrong_hash.success, "approve_spec should reject wrong spec hash");
    Expect(wrong_hash.error_message.find("does not match") != std::string::npos,
        "wrong hash approval should explain mismatch");

    const auto approved = store.approve_spec(submit.job.job_id, validated.spec_hash);
    Expect(approved.success, "approve_spec should approve hash-matched nonempty task spec");
    Expect(approved.job.status == "running", "approved spec should make job runnable");
    Expect(approved.job.phase == "codex_execution", "approved spec should enter codex_execution phase");
    Expect(approved.job.approval_gate == "none", "approved spec should clear approval gate");
    Expect(approved.job.next_action == "execute_next_task", "approved spec should point to execution adapter");

    const auto revision_status = nlohmann::json::parse(ReadFile(approved.status_path));
    Expect(revision_status["status"] == "approved_frozen",
        "approved spec should mark revision approved_frozen");
    Expect(revision_status["approved_by"] == "cli",
        "approved spec should record CLI approval provenance");
    Expect(std::filesystem::exists(approved.tasks_path),
        "approved spec should materialize runtime tasks.json");
    const auto tasks = nlohmann::json::parse(ReadFile(approved.tasks_path));
    Expect(tasks.is_array() && tasks.size() == 1,
        "approved spec should materialize one runtime task");
    Expect(tasks[0]["task_id"] == "task-001",
        "materialized runtime task should preserve task id");
    Expect(tasks[0]["status"] == "pending",
        "materialized runtime task should start pending");
    Expect(tasks[0]["current_activity"] == "none",
        "materialized runtime task should start with no activity");
    Expect(tasks[0]["spec_revision"] == "rev-001",
        "materialized runtime task should record source spec revision");
    Expect(tasks[0]["verify_command"] == "true",
        "materialized runtime task should preserve verify command");
    Expect(tasks[0]["acceptance_total"] == 1,
        "materialized runtime task should preserve acceptance count");
    std::string load_tasks_error;
    const auto loaded_tasks = store.load_tasks(submit.job.job_id, &load_tasks_error);
    Expect(loaded_tasks.has_value(), "load_tasks should read materialized runtime tasks");
    Expect(loaded_tasks.has_value() && loaded_tasks->size() == 1,
        "load_tasks should return materialized task count");
    Expect(loaded_tasks.has_value() && loaded_tasks->front().task_id == "task-001",
        "load_tasks should preserve task id");
    Expect(loaded_tasks.has_value() && loaded_tasks->front().status == "pending",
        "load_tasks should preserve pending task status");

    const auto events = ReadFile(store.events_path(submit.job.job_id));
    Expect(events.find("\"type\":\"autodev.spec.approved\"") != std::string::npos,
        "events.ndjson should record spec approved event");
    Expect(events.find("\"type\":\"autodev.tasks.materialized\"") != std::string::npos,
        "events.ndjson should record task materialization event");
    std::string load_events_error;
    const auto event_lines = store.load_event_lines(submit.job.job_id, &load_events_error);
    Expect(event_lines.has_value(), "load_event_lines should read append-only runtime event history");
    Expect(event_lines.has_value() && !event_lines->empty(),
        "load_event_lines should return runtime event lines");
}

void TestRecordExecutionBlockedAppendsAuditEventOnly() {
    const auto workspace = FreshWorkspace("execution_blocked_event");
    const auto target = workspace / "target_app";
    InitGitRepo(target);
    const auto skill_pack = workspace / "skills";
    CreateAutoDevSkillPackFixture(skill_pack);

    agentos::AutoDevStateStore store(workspace);
    const auto submit = store.submit(agentos::AutoDevSubmitRequest{
        .agentos_workspace = workspace,
        .target_repo_path = target,
        .objective = "Record execution blocked",
        .skill_pack_path = skill_pack,
    });
    Expect(submit.success, "submit before execution blocked event should succeed");
    const auto prepared = store.prepare_workspace(submit.job.job_id);
    Expect(prepared.success, "prepare_workspace before execution blocked event should succeed");
    const auto loaded = store.load_skill_pack(submit.job.job_id);
    Expect(loaded.success, "load_skill_pack before execution blocked event should succeed");
    const auto generated = store.generate_goal_docs(submit.job.job_id);
    Expect(generated.success, "generate_goal_docs before execution blocked event should succeed");

    const auto spec_path = prepared.job.job_worktree_path / "docs" / "goal" / "AUTODEV_SPEC.json";
    auto spec = nlohmann::json::parse(ReadFile(spec_path));
    spec["tasks"] = nlohmann::json::array({
        {
            {"task_id", "task-001"},
            {"title", "Execution blocked audit task"},
            {"allowed_files", nlohmann::json::array({"README.md"})},
            {"blocked_files", nlohmann::json::array({"package.json"})},
            {"verify_command", "true"},
            {"acceptance", nlohmann::json::array({"preflight only"})},
        }
    });
    {
        std::ofstream output(spec_path, std::ios::binary | std::ios::trunc);
        output << spec.dump(2) << '\n';
    }

    const auto validated = store.validate_spec(submit.job.job_id);
    Expect(validated.success, "validate_spec before execution blocked event should succeed");
    const auto approved = store.approve_spec(submit.job.job_id, validated.spec_hash);
    Expect(approved.success, "approve_spec before execution blocked event should succeed");
    std::string tasks_error;
    const auto tasks = store.load_tasks(submit.job.job_id, &tasks_error);
    Expect(tasks.has_value() && tasks->size() == 1,
        "execution blocked event fixture should have one materialized task");

    const auto profile = agentos::CodexCliAutoDevAdapterProfile();
    store.record_execution_blocked(approved.job, tasks->front(), profile, "adapter not implemented");
    const auto verified = store.verify_task(submit.job.job_id, "task-001", "turn-001");
    Expect(verified.success, "verify_task should run the materialized task verify_command");
    Expect(verified.verification.verification_id == "verify-001",
        "first verification should use verify-001");
    Expect(verified.verification.task_id == "task-001",
        "verification should record task id");
    Expect(verified.verification.command == "true",
        "verification should record verify command");
    Expect(verified.verification.cwd == approved.job.job_worktree_path,
        "verification should run in job worktree");
    Expect(verified.verification.exit_code == 0,
        "verification should record command exit code");
    Expect(verified.verification.passed,
        "verification should record passed=true for exit code zero");
    Expect(verified.verification.related_turn_id == "turn-001",
        "verification should link to related turn id when provided");
    Expect(std::filesystem::exists(store.verification_path(submit.job.job_id)),
        "verify_task should write verification.json under runtime store");
    Expect(verified.verification.output_log_path.has_value() &&
               std::filesystem::exists(*verified.verification.output_log_path),
        "verify_task should write command output log under runtime store");
    Expect(std::filesystem::exists(verified.verify_report_path),
        "verify_task should write VERIFY.md summary under job worktree");
    Expect(verified.verify_report_path == approved.job.job_worktree_path / "docs" / "goal" / "VERIFY.md",
        "verify_task should write VERIFY.md in job worktree goal docs");
    const auto verify_report = ReadFile(verified.verify_report_path);
    Expect(verify_report.find("Verification Report") != std::string::npos,
        "VERIFY.md should contain verification report heading");
    Expect(verify_report.find("It is NOT the source of truth for task completion") != std::string::npos,
        "VERIFY.md should state that it is not source of truth");
    Expect(verify_report.find("AcceptanceGate does not use this Markdown file") != std::string::npos,
        "VERIFY.md should state AcceptanceGate does not use markdown for judgment");
    Expect(verify_report.find("verify-001") != std::string::npos,
        "VERIFY.md should include verification id");
    std::string verifications_error;
    const auto verifications = store.load_verifications(submit.job.job_id, &verifications_error);
    Expect(verifications.has_value() && verifications->size() == 1,
        "load_verifications should read recorded verification facts");
    {
        std::ofstream readme(approved.job.job_worktree_path / "README.md", std::ios::binary | std::ios::app);
        readme << "allowed change\n";
    }
    const auto allowed_diff = store.diff_guard(submit.job.job_id, "task-001");
    Expect(allowed_diff.success, "diff_guard should inspect job worktree changes");
    Expect(allowed_diff.diff_guard.diff_id == "diff-001",
        "first diff guard check should use diff-001");
    Expect(allowed_diff.diff_guard.passed,
        "diff_guard should pass when only allowed files changed");
    Expect(!allowed_diff.diff_guard.changed_files.empty() &&
               allowed_diff.diff_guard.changed_files.front() == "README.md",
        "diff_guard should record changed file names");
    Expect(std::filesystem::exists(store.diffs_path(submit.job.job_id)),
        "diff_guard should write diffs.json under runtime store");
    std::string diffs_error;
    const auto diffs = store.load_diffs(submit.job.job_id, &diffs_error);
    Expect(diffs.has_value() && diffs->size() == 1,
        "load_diffs should read diff guard records");
    {
        const auto tasks_before_acceptance = nlohmann::json::parse(ReadFile(store.tasks_path(submit.job.job_id)));
        Expect(tasks_before_acceptance[0]["status"] == "pending",
            "recording verification and diff guard facts should not change task status before AcceptanceGate");
    }
    const auto accepted = store.acceptance_gate(submit.job.job_id, "task-001");
    Expect(accepted.success, "acceptance_gate should evaluate recorded runtime facts");
    Expect(accepted.acceptance.acceptance_id == "acceptance-001",
        "first acceptance gate check should use acceptance-001");
    Expect(accepted.acceptance.passed,
        "acceptance_gate should pass when latest verification and diff guard passed");
    Expect(accepted.acceptance.verification_id == "verify-001",
        "acceptance_gate should link latest verification fact");
    Expect(accepted.acceptance.diff_id == "diff-001",
        "acceptance_gate should link latest diff guard fact");
    Expect(accepted.task.status == "passed",
        "acceptance_gate should mark the task passed when runtime facts pass");
    Expect(std::filesystem::exists(store.acceptance_path(submit.job.job_id)),
        "acceptance_gate should write acceptance.json under runtime store");
    {
        const auto accepted_tasks = nlohmann::json::parse(ReadFile(store.tasks_path(submit.job.job_id)));
        Expect(accepted_tasks[0]["status"] == "passed",
            "acceptance_gate should persist passed task status");
        Expect(accepted_tasks[0]["acceptance_passed"] == accepted_tasks[0]["acceptance_total"],
            "acceptance_gate should mark all task acceptance criteria passed");
    }
    {
        std::ofstream blocked(approved.job.job_worktree_path / "package.json", std::ios::binary);
        blocked << "{}\n";
    }
    const auto blocked_diff = store.diff_guard(submit.job.job_id, "task-001");
    Expect(blocked_diff.success, "diff_guard should record violations as facts rather than failing command logic");
    Expect(!blocked_diff.diff_guard.passed,
        "diff_guard should fail when blocked files changed");
    Expect(!blocked_diff.diff_guard.blocked_file_violations.empty() &&
               blocked_diff.diff_guard.blocked_file_violations.front() == "package.json",
        "diff_guard should record blocked file violations");
    Expect(!blocked_diff.diff_guard.outside_allowed_files.empty() &&
               blocked_diff.diff_guard.outside_allowed_files.front() == "package.json",
        "diff_guard should record changes outside allowed files");

    Expect(std::filesystem::exists(store.turns_path(submit.job.job_id)),
        "recording execution blocked event should create turns.json");
    std::string turns_error;
    const auto turns = store.load_turns(submit.job.job_id, &turns_error);
    Expect(turns.has_value() && turns->size() == 1,
        "load_turns should read the blocked synthetic turn record");
    Expect(turns.has_value() && turns->front().turn_id == "turn-001",
        "first blocked synthetic turn should use turn-001");
    Expect(turns.has_value() && turns->front().status == "blocked",
        "blocked synthetic turn should record blocked status");
    Expect(turns.has_value() && turns->front().adapter_kind == "codex_cli",
        "blocked synthetic turn should record adapter kind");
    Expect(turns.has_value() && turns->front().continuity_mode == "best_effort_context",
        "blocked synthetic turn should record continuity mode");
    Expect(turns.has_value() && turns->front().event_stream_mode == "synthetic",
        "blocked synthetic turn should record event stream mode");
    Expect(turns.has_value() && turns->front().error_code == "execution_adapter_unavailable",
        "blocked synthetic turn should record an execution adapter unavailable error");
    Expect(turns.has_value() && turns->front().prompt_artifact.has_value() &&
               std::filesystem::exists(*turns->front().prompt_artifact),
        "blocked synthetic turn should write a runtime prompt artifact");
    Expect(turns.has_value() && turns->front().response_artifact.has_value() &&
               std::filesystem::exists(*turns->front().response_artifact),
        "blocked synthetic turn should write a runtime response artifact");
    if (turns.has_value() && turns->front().prompt_artifact.has_value()) {
        const auto prompt = ReadFile(*turns->front().prompt_artifact);
        Expect(prompt.find("AutoDev Execution Turn Prompt") != std::string::npos,
            "prompt artifact should identify the execution prompt");
        Expect(prompt.find("Execution blocked audit task") != std::string::npos,
            "prompt artifact should include task title");
        Expect(prompt.find("AcceptanceGate") != std::string::npos,
            "prompt artifact should preserve AcceptanceGate authority instructions");
    }
    if (turns.has_value() && turns->front().response_artifact.has_value()) {
        const auto response = ReadFile(*turns->front().response_artifact);
        Expect(response.find("Execution was not started") != std::string::npos,
            "response artifact should state that execution was not started");
        Expect(response.find("adapter not implemented") != std::string::npos,
            "response artifact should record the blocked reason");
    }

    const auto events = ReadFile(store.events_path(submit.job.job_id));
    Expect(events.find("\"type\":\"autodev.execution.blocked\"") != std::string::npos,
        "events.ndjson should record execution blocked event");
    Expect(events.find("\"turn_id\":\"turn-001\"") != std::string::npos,
        "execution blocked event should link to the synthetic turn record");
    Expect(events.find("\"prompt_artifact\"") != std::string::npos,
        "execution blocked event should link to prompt artifact");
    Expect(events.find("\"response_artifact\"") != std::string::npos,
        "execution blocked event should link to response artifact");
    Expect(events.find("\"type\":\"autodev.verification.started\"") != std::string::npos,
        "events.ndjson should record verification started event");
    Expect(events.find("\"type\":\"autodev.verification.completed\"") != std::string::npos,
        "events.ndjson should record verification completed event");
    Expect(events.find("\"type\":\"autodev.diff_guard.started\"") != std::string::npos,
        "events.ndjson should record diff guard started event");
    Expect(events.find("\"type\":\"autodev.diff_guard.completed\"") != std::string::npos,
        "events.ndjson should record diff guard completed event");
    Expect(events.find("\"type\":\"autodev.acceptance_gate.completed\"") != std::string::npos,
        "events.ndjson should record acceptance gate completed event");
    Expect(events.find("\"verify_report_path\"") != std::string::npos,
        "verification completed event should link VERIFY.md report path");
    Expect(events.find("\"adapter_kind\":\"codex_cli\"") != std::string::npos,
        "execution blocked event should record adapter kind");
    Expect(events.find("\"event_stream_mode\":\"synthetic\"") != std::string::npos,
        "execution blocked event should record adapter event stream mode");
    const auto persisted_tasks = nlohmann::json::parse(ReadFile(store.tasks_path(submit.job.job_id)));
    Expect(persisted_tasks[0]["status"] == "passed",
        "AcceptanceGate should be the first runtime step that changes task status");
    const auto persisted_job = nlohmann::json::parse(ReadFile(store.job_json_path(submit.job.job_id)));
    Expect(persisted_job["status"] == "running",
        "acceptance_gate should not mark the job done");
}

}  // namespace

int main() {
    TestJobIdValidation();
    TestCodexCliExecutionAdapterProfileIsTransitional();
    TestSubmitCreatesRuntimeFactsOnly();
    TestSubmitWithoutSkillPackStaysNotLoaded();
    TestSubmitMissingTargetFailsWithoutJob();
    TestLoadRejectsInvalidJobId();
    TestPrepareWorkspaceCreatesGitWorktree();
    TestPrepareWorkspaceBlocksDirtyTarget();
    TestLoadSkillPackRecordsSnapshotOnly();
    TestLoadSkillPackBlocksMissingSteps();
    TestGenerateGoalDocsWritesOnlyJobWorktree();
    TestGenerateGoalDocsBlocksUntilReady();
    TestValidateSpecSnapshotsPendingRevision();
    TestValidateSpecBlocksUnsupportedSchema();
    TestApproveSpecRequiresNonEmptyTasks();
    TestApproveSpecFreezesHashBoundRevision();
    TestRecordExecutionBlockedAppendsAuditEventOnly();

    if (failures != 0) {
        std::cerr << failures << " AutoDev test assertion(s) failed\n";
        return 1;
    }
    std::cout << "AutoDev tests passed\n";
    return 0;
}
