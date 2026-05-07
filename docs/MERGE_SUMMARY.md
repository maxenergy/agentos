# AgentOS Branch Merge Summary

Branch: `codex/improve-interactive-routing`

Base checked against: `origin/main`

As of 2026-05-07, this branch had no commits behind `origin/main` when checked
locally. Recheck before merging with:

```bash
git fetch origin main codex/improve-interactive-routing
git rev-list --count origin/main..HEAD
git rev-list --count HEAD..origin/main
git diff --shortstat origin/main...HEAD
```

## Scope

This branch contains two major work streams.

### AutoDev Runtime

The AutoDev stream adds the MVP runtime for autonomous development jobs:

- job submission, job ids, runtime models, and workspace state store;
- worktree preparation and skill-pack metadata loading;
- generated goal docs and hash-bound `AUTODEV_SPEC` approval;
- task materialization, task/event/fact listing, and JSON query output;
- execution adapters for preflight, Codex CLI, and a minimal app-server
  transport;
- verification, DiffGuard, AcceptanceGate, FinalReview, and explicit job
  completion gates;
- pre-task snapshots, rollback facts, repair-needed facts, repair prompt
  artifacts, retry limits, pause/resume/cancel, crash recovery, status watch,
  progress output, dashboard output, and worktree cleanup policies;
- operator docs in `docs/AUTODEV.md`, `docs/AUTODEV_COOKBOOK.md`, ADR
  `0001-autodev-runtime-store-ownership`, and the spec schema at
  `docs/schemas/AUTODEV_SPEC.schema.json`.

Authority boundary:

- AgentOS runtime files under `runtime/autodev/jobs/<job_id>/` are the source
  of truth.
- Worktree `docs/goal/*` files are human/model-facing artifacts only.
- Codex execution output cannot mark tasks or jobs passed by editing files.
- AgentOS gates own task acceptance, final review, and job completion.

### Main REPL Routing

The REPL stream changes interactive routing from keyword-final local dispatch
to a context-aware main-agent orchestration path:

- free-form natural-language turns go through the configured `main` agent;
- hard local commands remain local;
- `main` receives live capability hints plus a compact REPL context digest;
- `main` can answer directly or emit a validated `agentos_route_action`;
- route actions execute through registered skills/agents and return compact
  results to `main` for synthesis;
- missing route-action inputs keep pending route state for follow-up turns;
- high-risk route actions require the existing approval path;
- REPL context persists across restarts and supports named contexts;
- context commands include `show`, `clear`, `list`, `use`, `delete`, `rename`,
  `export`, `privacy`, and `trace`;
- default context privacy is `digest`; `none` and `verbatim` are available per
  named context;
- routing trace records are metadata-only JSONL and can be inspected with
  `context trace tail [n] [--pretty]`;
- helper code is split into `interactive_chat_state.*`,
  `interactive_main_context.*`, and `interactive_route_action_executor.*`.

User-facing docs:

- `docs/MAIN_REPL.md`
- `docs/MAIN_REPL_ROUTING_REFACTOR_PLAN.md`
- `docs/MAIN_REPL_ROUTING_STATUS.md`

## Verification

Latest verification run on this branch:

```bash
git diff --check
cmake --build build
ctest --test-dir build --output-on-failure -R "agentos_cli_integration_tests|agentos_main_agent_prompt_tests|agentos_intent_classifier_tests|agentos_routing_eval_tests|agentos_interactive_chat_state_tests"
ctest --test-dir build --output-on-failure
```

Results:

- Build passed.
- Focused REPL routing tests passed: 5/5.
- Full CTest suite passed: 25/25.
- `git diff --check` passed.

## Merge Notes

- The branch currently has no commits behind `origin/main`.
- Existing unrelated untracked local files are not part of this branch:
  `AUTODEV_AGENTOS_SKILLS_DESIGN.md`, `docs/use-cases/`, and `run_codex.sh`.
- `runtime/` state files remain local/generated state and should not be
  committed unless deliberately adding specs or fixtures.
- Main REPL continuation tests use neutral batch-cadence examples so routing
  regression coverage does not depend on business-specific prompt text.

## Suggested Review Order

1. Review AutoDev runtime data model and state-store ownership docs.
2. Review AutoDev CLI commands and focused `agentos_autodev_tests` coverage.
3. Review Main REPL routing docs and `main_route_action` validation.
4. Review interactive integration tests for continuation, context persistence,
   route-action execution, high-risk approval, privacy, and trace commands.
5. Run the verification commands above before merging.
