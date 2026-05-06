# ADR 0001: AutoDev runtime facts live in AgentOS Runtime Store

## Status

Accepted

## Date

2026-05-06

## Context

AutoDev introduces long-lived software engineering jobs with their own job, task, turn, verification, diff, rollback, and acceptance state.

A target project may be located at:

```text
/home/rogers/source/app
```

AgentOS may create an isolated worktree for a job:

```text
/home/rogers/source/app-autodev-a1b2c3
```

AgentOS itself runs from:

```text
/home/rogers/source/agentos
```

A key architectural decision is where AutoDev's factual runtime state should live.

A natural option is to place `runtime/autodev` inside the target repo or job worktree. This would make state visible near the code, but it would also pollute the target project, risk being included in PRs, and allow Codex or other code-writing tools to modify the facts used for acceptance.

AutoDev requires a stricter authority boundary:

- Codex may read and modify files in the job worktree.
- AgentOS must own the factual job state.
- AcceptanceGate must not rely on files that Codex can modify.
- Runtime evidence and audit history must survive even if the worktree is reset, cleaned, or discarded.

## Decision

AutoDev runtime facts are stored in AgentOS Runtime Store, under:

```text
<agentos_workspace>/runtime/autodev/jobs/<job_id>/
```

For example:

```text
<agentos_workspace>/runtime/autodev/jobs/autodev-20260506-164233-a1b2c3/
  job.json
  events.ndjson
  tasks.json
  turns.json
  acceptance.json
  verification.json
  diffs.json
  rollbacks.json
  artifacts/
  logs/
  spec_revisions/
  snapshots/
```

The target repo and job worktree must not contain AgentOS runtime facts.

The job worktree may contain human/model-facing AutoDev artifacts such as:

```text
<job_worktree_path>/docs/goal/REQUIREMENTS.md
<job_worktree_path>/docs/goal/DESIGN.md
<job_worktree_path>/docs/goal/NON_GOALS.md
<job_worktree_path>/docs/goal/IMPACT.md
<job_worktree_path>/docs/goal/TASKS.md
<job_worktree_path>/docs/goal/ACCEPTANCE.md
<job_worktree_path>/docs/goal/GOAL.md
<job_worktree_path>/docs/goal/CODEX_START.md
<job_worktree_path>/docs/goal/AUTODEV_SPEC.json
<job_worktree_path>/docs/goal/VERIFY.md
<job_worktree_path>/docs/goal/FINAL_REVIEW.md
```

However, these worktree files are not the source of authority for AgentOS completion decisions.

AgentOS must validate, normalize, snapshot, and hash `AUTODEV_SPEC.json` before execution. AcceptanceGate must use the AgentOS runtime snapshot, not the mutable worktree file.

For example:

```text
<agentos_workspace>/runtime/autodev/jobs/<job_id>/spec_revisions/rev-001.normalized.json
<agentos_workspace>/runtime/autodev/jobs/<job_id>/spec_revisions/rev-001.sha256
```

Similarly:

- `verification.json` is the source of truth for verification results.
- `final_review.json` is the source of truth for final review results.
- `VERIFY.md` and `FINAL_REVIEW.md` are human-readable summaries only.
- `events.ndjson` is the append-only runtime event history.
- `job.json` is the current job fact snapshot.

Codex may read `docs/goal/*` for context, but Codex must not be able to change job status, task status, verification results, acceptance results, or final completion by editing files in the worktree.

## Consequences

### Positive

- AgentOS remains the authority for AutoDev facts.
- AcceptanceGate cannot be bypassed by modifying worktree Markdown or JSON files.
- Target repos are not polluted with runtime job state.
- PRs do not accidentally include internal execution state.
- Runtime logs, verification history, rollback records, and event history survive worktree cleanup.
- Multiple AutoDev jobs can be tracked independently even if they operate on different worktrees.
- Future UI, scheduler, ravbot, or API layers can query AgentOS Runtime Store directly.

### Negative

- Users cannot inspect all AutoDev facts by only looking inside the target worktree.
- Status and audit queries must go through AgentOS.
- AgentOS must maintain runtime store paths, snapshots, hashes, and event records.
- Worktree `docs/goal` files and AgentOS runtime facts may diverge unless explicitly synchronized.

### Mitigations

- AgentOS may write human-readable summaries to the worktree:
  - `docs/goal/VERIFY.md`
  - `docs/goal/FINAL_REVIEW.md`

- These summaries must clearly state that they are not the source of truth.

- Status commands should expose runtime facts:

  ```bash
  agentos autodev status job_id=<job_id>
  agentos autodev events job_id=<job_id>
  ```

- PR bodies can include summaries copied from AgentOS runtime facts without committing runtime files.

## Invariants

- AutoDev runtime facts live under `<agentos_workspace>/runtime/autodev/jobs/<job_id>/`.
- `target_repo_path` is read-only by default.
- `job_worktree_path` contains code changes and optional `docs/goal` artifacts.
- AcceptanceGate reads AgentOS runtime facts only.
- Codex cannot mark a task passed or a job done by modifying worktree files.
- `AUTODEV_SPEC.json` in the worktree is a candidate/spec artifact.
- AgentOS normalized spec snapshots are the execution authority.
- `VERIFY.md` and `FINAL_REVIEW.md` are summaries, not authority.
- `events.ndjson` records append-only history.
- `job.json` records current fact state.

## Related decisions kept in CONTEXT.md

The following related decisions are currently documented in `CONTEXT.md` and do not need separate ADRs yet:

- AutoDev is a new top-level subsystem, not a rename of `development_request`.
- AutoDev MVP 1 uses top-level CLI submit/status only.
- AutoDev MVP 1 records planned worktree path but does not create worktree.
- Status, phase, and current activity are separate state concepts.
- SkillPackLoader is deferred to MVP 2 but skill pack binding is reserved in the job model.
- MVP 4 may use a transitional Codex CLI execution adapter, but the execution interface is designed for Codex app-server.
