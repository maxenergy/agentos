# AutoDev Runtime State

AutoDev runtime facts live in the AgentOS workspace, not in the target repo or job worktree:

```text
<agentos_workspace>/runtime/autodev/jobs/<job_id>/
```

The job worktree may contain human-facing `docs/goal/*` artifacts, but those files are not authoritative for task acceptance or job completion. AgentOS runtime JSON/NDJSON files are the source of truth.

Generated goal docs include `VERIFY.template.md`, `FINAL_REVIEW.template.md`, and `TASK.template.md` to guide human/model-written evidence. These templates are not runtime facts and cannot change task or job status.

## Runtime Files

| Path | Purpose | CLI |
|---|---|---|
| `job.json` | Current job state: status, phase, next action, worktree paths, spec hash, blocker, timestamps. | `agentos autodev status job_id=<job_id>` |
| `events.ndjson` | Append-only audit/event history for the job. | `agentos autodev events job_id=<job_id>` |
| `tasks.json` | Materialized frozen tasks from the approved spec, including allowed/blocked files, acceptance counts, and retry counters. | `agentos autodev tasks job_id=<job_id>` |
| `turns.json` | Execution turn records, including synthetic blocked turns for fail-closed execution paths. | `agentos autodev turns job_id=<job_id>` |
| `verification.json` | Verification facts from AgentOS-run `verify_command` executions. | `agentos autodev verifications job_id=<job_id>` |
| `diffs.json` | DiffGuard facts from the job worktree, including changed files, blocked-file violations, and outside-allowed files. | `agentos autodev diffs job_id=<job_id>` |
| `acceptance.json` | AcceptanceGate decisions linking latest verification and diff facts per task. | `agentos autodev acceptances job_id=<job_id>` |
| `final_review.json` | FinalReview decisions for the job, including task totals, changed files, policy violations, and reasons. | `agentos autodev final-reviews job_id=<job_id>` |
| `snapshots.json` | Pre-task snapshot index with HEAD, git status, task id, timestamp, and artifact path. | `agentos autodev snapshots job_id=<job_id>` |
| `snapshots/<snapshot_id>.json` | Per-snapshot artifact containing the same snapshot fact. | `agentos autodev snapshots job_id=<job_id>` |
| `rollbacks.json` | Rollback intent/result facts for soft and hard rollback requests. | `agentos autodev rollbacks job_id=<job_id>` |
| `repairs.json` | Repair-needed facts from failed verification, diff, or acceptance gates, including retry state and prompt artifact path. | `agentos autodev repairs job_id=<job_id>` |
| `repairs/<repair_id>.prompt.md` | Same-thread repair prompt artifact generated from failed runtime facts. | `agentos autodev repair-next job_id=<job_id>` |
| `logs/<verification_id>.output.txt` | Captured verification command output. | `agentos autodev verifications job_id=<job_id>` |
| `prompts/<turn_id>.md` | Execution prompt artifact for a recorded turn. | `agentos autodev turns job_id=<job_id>` |
| `responses/<turn_id>.md` | Execution response/blocker artifact for a recorded turn. | `agentos autodev turns job_id=<job_id>` |
| `spec_revisions/<rev>.normalized.json` | Normalized approved spec candidate snapshot. | `agentos autodev validate-spec job_id=<job_id>` |
| `spec_revisions/<rev>.sha256` | Hash of the normalized spec snapshot. | `agentos autodev validate-spec job_id=<job_id>` |
| `spec_revisions/<rev>.status.json` | Spec revision approval status. | `agentos autodev approve-spec job_id=<job_id> spec_hash=<sha256>` |

## Machine-Readable Queries

The following commands support `format=json`:

```bash
agentos autodev summary job_id=<job_id> format=json
agentos autodev tasks job_id=<job_id> format=json
agentos autodev verifications job_id=<job_id> format=json
agentos autodev diffs job_id=<job_id> format=json
agentos autodev acceptances job_id=<job_id> format=json
agentos autodev final-reviews job_id=<job_id> format=json
```

`summary format=json` includes:

```text
job
progress
fact_counts
tasks
snapshots
verifications
diffs
acceptances
final_reviews
repairs
```

## AUTODEV_SPEC Schema

The repository carries the current AutoDev spec schema at:

```text
docs/schemas/AUTODEV_SPEC.schema.json
```

`agentos autodev validate-spec` enforces the supported `schema_version` and the core shape used by the runtime before it snapshots a pending spec revision. Empty `tasks` are allowed at validation time so generated goal skeletons can be reviewed, but `approve-spec` blocks execution until tasks can be materialized.

## Blocked Recovery

`agentos autodev recover-blocked job_id=<job_id> [skill_pack_path=<path>]` is a conservative recovery helper for blocked setup gates. It reruns the recoverable runtime gate implied by `next_action`, such as workspace preparation after the target repo is cleaned, skill-pack loading after a path or manifest fix, goal-doc generation after prerequisites are ready, or spec validation after `AUTODEV_SPEC.json` is fixed.

The command does not approve specs, mark tasks passed, run Codex execution, or mark jobs done. Jobs in `awaiting_approval` still require an explicit hash-bound `approve-spec` command.

## Execution Adapter

`agentos autodev execute-next-task job_id=<job_id> execution_adapter=codex_cli` records a pre-task snapshot, builds a task prompt, runs the configured Codex CLI command in the job worktree, and records a turn fact with prompt artifact, response artifact, exit code, duration, and changed files. The command defaults to `codex exec --skip-git-repo-check --sandbox workspace-write -`; callers can override it with `codex_cli_command=<command>` or `AGENTOS_AUTODEV_CODEX_CLI_COMMAND`.

`execution_adapter=codex_app_server` supports a minimal HTTP transport when `app_server_url=<url>` or `AGENTOS_AUTODEV_CODEX_APP_SERVER_URL` is provided. AgentOS checks `GET /health`, opens a session with `POST /sessions`, submits a turn to `POST /sessions/<session_id>/turns`, and records the returned output/events in the execution response artifact. Without a URL, the app-server adapter remains fail-closed.

Execution turn records do not mark tasks passed. Task state remains controlled by `verify-task`, `diff-guard`, and `acceptance-gate`.

`agentos autodev run-task job_id=<job_id>` is the single-task pipeline wrapper for the next pending task. It records the pre-task snapshot through `execute-next-task`, then runs `verify-task`, `diff-guard`, and `acceptance-gate` for the executed task. The same adapter options accepted by `execute-next-task` are supported. The command stops at the first failed stage; failed verification, DiffGuard, or AcceptanceGate stages leave the existing repair-needed facts and print the repair entrypoint.

## Repair Flow

Failed verification, DiffGuard, AcceptanceGate, and FinalReview checks can record repair-needed facts in `repairs.json` and write a same-thread repair prompt under `repairs/<repair_id>.prompt.md`.

`agentos autodev repair-next job_id=<job_id>` selects the latest actionable repair, reads its prompt artifact, and prints a same-task repair flow. `agentos autodev repair-task job_id=<job_id> task_id=<task_id>` does the same for a specific task. These commands are read-only: they do not run Codex, mutate task status, reset retry counters, or mark repairs completed.

## Authority Boundary

Codex may read and modify files under the job worktree, including `docs/goal/*`. Codex cannot mark a task passed or a job done by editing worktree files.

Only AgentOS runtime gates mutate authoritative state:

- `execute-next-task` may run a configured execution adapter and record turn facts; it does not mark tasks passed.
- `repair-next` and `repair-task` may read repair facts and prompt artifacts; they do not mutate authoritative state.
- `verify-task` records verification facts only.
- `diff-guard` records diff facts only.
- `acceptance-gate` may mark a task passed when latest verification and diff facts pass.
- `final-review` may advance a job to `pr_ready`.
- `complete-job` / `mark-done` may advance a `pr_ready` job with latest passed final review to `done`.
- `recover-blocked` may rerun setup gates selected from runtime `next_action`; it cannot approve specs or start execution.
- `cleanup-worktree` may remove a job worktree only after the job is `done` or `cancelled`; runtime facts remain in the AgentOS store.
- `pause`, `resume`, and `cancel` currently mutate job state and append events only; they do not interrupt or terminate a Codex process.
