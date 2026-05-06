# AutoDev Runtime State

AutoDev runtime facts live in the AgentOS workspace, not in the target repo or job worktree:

```text
<agentos_workspace>/runtime/autodev/jobs/<job_id>/
```

The job worktree may contain human-facing `docs/goal/*` artifacts, but those files are not authoritative for task acceptance or job completion. AgentOS runtime JSON/NDJSON files are the source of truth.

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
| `repairs/<repair_id>.prompt.md` | Same-thread repair prompt artifact generated from failed runtime facts. | `agentos autodev repairs job_id=<job_id>` |
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

## Authority Boundary

Codex may read and modify files under the job worktree, including `docs/goal/*`. Codex cannot mark a task passed or a job done by editing worktree files.

Only AgentOS runtime gates mutate authoritative state:

- `verify-task` records verification facts only.
- `diff-guard` records diff facts only.
- `acceptance-gate` may mark a task passed when latest verification and diff facts pass.
- `final-review` may advance a job to `pr_ready`.
- `complete-job` / `mark-done` may advance a `pr_ready` job with latest passed final review to `done`.
- `pause`, `resume`, and `cancel` currently mutate job state and append events only; they do not interrupt or terminate a Codex process.

