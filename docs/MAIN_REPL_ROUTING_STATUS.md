# Main REPL Routing Status

Status: merge-ready on branch `codex/improve-interactive-routing`.

## Completed Capabilities

- Free-form REPL text routes through the configured `main` agent instead of
  being finally owned by local research/development keyword routing.
- Hard local commands remain local: `help`, `status`, `run`, `agents`,
  `skills`, `jobs`, `memory`, `schedule`, and `context`.
- `main` receives live capability hints and may either answer directly or emit
  an `agentos_route_action`.
- Route actions are validated against live skill/agent registries and skill
  input schemas before execution.
- Missing route-action inputs are handled as a follow-up loop with pending
  route-action state.
- High-risk route actions require the existing AgentOS approval flow.
- REPL chat context persists across restarts under
  `runtime/main_agent/sessions/<name>.json`.
- Named contexts are supported with `context use <name>` and `context list`.
- Named context lifecycle commands are available:
  `context delete <name>`, `context rename <old> <new>`, and
  `context export <name> [path]`.
- Context privacy is configurable per named context:
  `digest`, `none`, or `verbatim`.
- Default `digest` mode sends a sanitized continuity digest instead of full
  prior turns.
- Main routing trace records are written to
  `runtime/main_agent/routing_trace.jsonl`.
- Trace can be inspected or cleared in the REPL:
  `context trace tail [n] [--pretty]` and `context trace clear`.
- User-facing documentation lives in `docs/MAIN_REPL.md`.
- REPL helper code has been split out:
  `interactive_chat_state.*`, `interactive_main_context.*`, and
  `interactive_route_action_executor.*`.

## Verification

Latest full verification on this branch:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

Result:

- Build passed.
- Full CTest suite passed: `25/25`.
- `git diff --check` passed.

Focused coverage includes:

- `agentos_cli_integration_tests`
- `agentos_intent_classifier_tests`
- `agentos_routing_eval_tests`
- `agentos_main_agent_prompt_tests`
- `agentos_main_route_action_tests`
- `agentos_interactive_chat_state_tests`

## Merge Readiness

This branch is ready to merge from the REPL routing perspective.

Expected user-visible changes:

- Natural-language follow-ups should stay in the main conversational context
  unless `main` explicitly asks AgentOS to call a registered capability.
- Users can inspect and control main-agent context with `context ...` commands.
- Operators can debug misrouting from `context trace tail --pretty` without
  reading raw prompt bodies.
- Default privacy is safer: `digest` sends a sanitized summary rather than
  full local transcript history.

Known non-blocking notes:

- `verbatim` privacy intentionally exists for local debugging and should not be
  the default.
- `routing_trace.jsonl` records routing metadata, not user prompt text.
- Existing unrelated untracked local files are not part of this branch.

## Optional Follow-ups

- Add more polished examples to `docs/MAIN_REPL.md` after the REPL UX settles.
- Move additional top-level REPL command families into modules if
  `interactive_commands.cpp` needs another cleanup pass.
- Add a route-action result object type if route-action synthesis grows beyond
  the current prompt-based handoff.
