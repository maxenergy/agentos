# AutoDev Cookbook

This cookbook shows the operator flow for one AutoDev job. Runtime JSON files under
`runtime/autodev/jobs/<job_id>/` are the authority for status; worktree Markdown is
only human/model-facing evidence.

## 1. Submit

```bash
agentos autodev submit \
  target_repo_path=/path/to/repo \
  objective="Implement the requested change" \
  skill_pack_path=/path/to/skills \
  worktree_cleanup_policy=keep_until_done
```

Save the printed `job_id`. Check current state at any point:

```bash
agentos autodev status job_id=<job_id>
agentos autodev summary job_id=<job_id>
agentos autodev events job_id=<job_id> format=json
```

## 2. Prepare And Freeze Spec

```bash
agentos autodev prepare-workspace job_id=<job_id>
agentos autodev load-skill-pack job_id=<job_id>
agentos autodev generate-goal-docs job_id=<job_id>
```

Edit and review the generated `docs/goal/AUTODEV_SPEC.json` in the job worktree.
Then validate it:

```bash
agentos autodev validate-spec job_id=<job_id>
```

Approve exactly the hash printed by validation:

```bash
agentos autodev approve-spec job_id=<job_id> spec_hash=<sha256>
```

## 3. Execute Tasks

Run one pending task through execution, verification, DiffGuard, and AcceptanceGate:

```bash
agentos autodev run-task job_id=<job_id> execution_adapter=codex_cli
```

Run every pending task until the job reaches `final_review`:

```bash
agentos autodev run-job job_id=<job_id> execution_adapter=codex_cli
```

Use a custom Codex CLI command when needed:

```bash
agentos autodev run-task \
  job_id=<job_id> \
  codex_cli_command="codex exec --skip-git-repo-check --sandbox workspace-write -"
```

For a Codex app-server transport:

```bash
agentos autodev run-task \
  job_id=<job_id> \
  execution_adapter=codex_app_server \
  app_server_url=http://127.0.0.1:18081
```

## 4. Manual Gate Flow

For low-level debugging, run the gates separately:

```bash
agentos autodev execute-next-task job_id=<job_id> execution_adapter=codex_cli
agentos autodev turns job_id=<job_id>

agentos autodev verify-task job_id=<job_id> task_id=<task_id> related_turn_id=<turn_id>
agentos autodev diff-guard job_id=<job_id> task_id=<task_id>
agentos autodev acceptance-gate job_id=<job_id> task_id=<task_id>
```

Inspect facts:

```bash
agentos autodev verifications job_id=<job_id> format=json
agentos autodev diffs job_id=<job_id> format=json
agentos autodev acceptances job_id=<job_id> format=json
```

## 5. Repair Failed Gates

When verification, DiffGuard, AcceptanceGate, or FinalReview records a repair:

```bash
agentos autodev repairs job_id=<job_id>
agentos autodev repair-next job_id=<job_id>
```

For a specific task:

```bash
agentos autodev repair-task job_id=<job_id> task_id=<task_id>
```

Apply the prompt in the same Codex thread/session, then rerun `run-task` or the
manual gate flow for that same task.

## 6. Pause, Cancel, And Recover

Pause or cancel from another terminal:

```bash
agentos autodev pause job_id=<job_id>
agentos autodev resume job_id=<job_id>
agentos autodev cancel job_id=<job_id>
```

`pause` and `cancel` are observed by the Codex CLI execution path and terminate a
running adapter process. `resume` does not restart the interrupted process; rerun
the task command after inspecting facts.

Recover setup blockers:

```bash
agentos autodev recover-blocked job_id=<job_id>
```

Recover crash-sensitive runtime state:

```bash
agentos autodev recover-crash job_id=<job_id>
```

## 7. Final Review And Done

When all tasks are accepted and the job is in `final_review`:

```bash
agentos autodev final-review job_id=<job_id>
agentos autodev pr-summary job_id=<job_id>
```

If final review passes, mark the job done:

```bash
agentos autodev mark-done job_id=<job_id>
```

`mark-done` is the only command that changes a `pr_ready` job to `done`.

## 8. Cleanup

For `worktree_cleanup_policy=keep_until_done`, cleanup is explicit:

```bash
agentos autodev cleanup-worktree job_id=<job_id>
```

For `delete_on_done`, cleanup runs during `mark-done`.

For `keep_always`, cleanup is refused and the worktree is retained.

Runtime facts remain under `runtime/autodev/jobs/<job_id>/` after cleanup.

## 9. Watcher Queries

Text status:

```bash
agentos autodev watch job_id=<job_id> iterations=10 interval_ms=1000
```

Filtered event feed:

```bash
agentos autodev events job_id=<job_id> format=json type=autodev.diff_guard.completed
agentos autodev events job_id=<job_id> format=json since=2026-05-07T00:00:00Z
```
