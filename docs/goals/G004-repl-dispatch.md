# G004 REPL Main-First Dispatch

Status: done
Depends on: G002

## Objective

Keep hard REPL commands local, but route free-form natural language to the
configured `main` agent first. `main` may answer directly or request a
structured AgentOS route action when a registered skill/agent is materially
needed.

## Context

Read `CONTEXT.md` terms:

- Agent Dispatch
- Capability Declaration
- Route Hint
- Capability Contract

## Scope

Allowed files:

- `src/cli/interactive_commands.cpp`
- `src/cli/interactive_commands.hpp`
- `src/cli/intent_classifier.*`
- `src/cli/main_route_action.*`
- `src/skills/builtin/development_skill.*`
- `src/skills/builtin/research_skill.*`
- `tests/cli_integration_tests.cpp`
- `tests/main_route_action_tests.cpp`

Out of scope:

- Adding `runtime/skill_routes.tsv` as a separate route-hint source.
- Agent Dispatch seam work.
- `learn_skill` implementation.

## Requirements

- After slash-command dispatch, REPL calls `ClassifyInteractiveRequest` only
  for hard-local intent detection and route-decision audit.
- Free-form natural language routes to `chat_agent -> main`.
- The `main` prompt receives live registered skills/agents, recent REPL chat
  context, and the structured `agentos_route_action` contract.
- If `main` emits a route action, the REPL executes the requested registered
  target through normal AgentOS routing and sends the compact result back to
  `main` for synthesis.
- Route actions are validated against live registries and skill input schemas
  before execution.
- Missing required inputs are returned to `main` as clarification-oriented
  tool results; the REPL keeps a pending route action for the next user turn.
- Routed action executions remain visible in audit and memory like normal
  tasks.

## Acceptance

- Routing eval coverage proves research/development-shaped natural language
  stays on `main` instead of being preempted by the REPL.
- Main prompt coverage proves recent context and the structured route action
  contract are present.
- Route action coverage proves invalid targets, missing inputs, high-risk
  actions, and missing-input follow-up context are handled.
- No `runtime/skill_routes.tsv` loader is introduced unless a separate
  lifecycle is explicitly justified.

## Verification

```bash
cmake --build build --target agentos_cli_integration_tests
ctest --test-dir build -R agentos_cli_integration_tests --output-on-failure
git diff --check
```

Completed verification:

- `ctest --test-dir build --output-on-failure -R "agentos_cli_integration_tests|agentos_main_route_action_tests|agentos_main_agent_prompt_tests|agentos_intent_classifier_tests|agentos_routing_eval_tests"`
- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `git diff --check`

Latest result: focused tests passed, full suite passed `24/24`, and
`git diff --check` passed.
