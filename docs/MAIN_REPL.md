# Main REPL

`agentos interactive` is the primary conversational shell. It keeps hard CLI
commands local, sends free-form conversation to the configured `main` agent,
and lets `main` decide whether to answer directly or request a registered
AgentOS capability.

## Routing Model

- Local commands stay local: `help`, `status`, `run`, `agents`, `skills`,
  `memory`, `schedule`, `jobs`, and `context`.
- Free-form text routes to `chat_agent -> main`.
- The local classifier records a route-decision artifact but does not own
  development/research delegation for ordinary conversation.
- `main` receives live skill/agent capability hints and a route-action
  contract. If it needs a registered capability, it can emit an
  `agentos_route_action`.
- The REPL validates that route action, executes the target through the normal
  AgentOS loop, then sends the compact result back to `main` for synthesis.
- Missing required capability inputs keep a pending route action so a short
  follow-up can complete the prior request.

## Context Commands

```text
context show
context clear
context list
context use <name>
context privacy [digest|none|verbatim]
context trace tail [n] [--pretty]
context trace clear
```

`context show` prints the current local transcript. `context clear` clears the
current transcript and pending route action. `context list` shows named
contexts and turn counts. `context use <name>` switches to another persisted
conversation.

Named context state is stored under:

```text
runtime/main_agent/current_context.txt
runtime/main_agent/sessions/<name>.json
runtime/main_agent/privacy/<name>.txt
```

## Context Privacy

Each named context has a privacy level. The default is `digest`.

- `digest`: send a compact sanitized continuity digest to `main`.
- `none`: send no previous chat context to `main`.
- `verbatim`: send recent turns as provider-native prior messages. Use this
  only for local debugging when exact prior wording matters.

The full transcript remains local for `context show` and persisted session
files. In `digest` mode, the external `main` request receives a short
`[REPL CONTEXT DIGEST]`, not the full local transcript.

Check or change the current level:

```text
context privacy
context privacy none
context privacy digest
context privacy verbatim
```

`status` shows the active context and privacy level:

```text
main_context: repl-default
main_context_turns: 3
main_context_privacy: digest
main_routing_trace: runtime/main_agent/routing_trace.jsonl
```

## Routing Trace

Main routing trace records are appended to:

```text
runtime/main_agent/routing_trace.jsonl
```

Trace records do not store the user prompt text. They record routing metadata:

- context privacy level;
- whether conversation context was sent;
- whether a pending route action was sent;
- whether `main` requested a route action;
- route-action target and execution result;
- pending-action state after execution.

Inspect recent trace records:

```text
context trace tail
context trace tail 20
context trace tail --pretty
context trace tail 20 --pretty
```

Clear the trace:

```text
context trace clear
```

Pretty output is intended for quick diagnosis. Raw JSONL remains the default
for scripts and exact inspection.

## Misrouting Debug Checklist

1. Run `status` and confirm `main_context` and `main_context_privacy`.
2. Run `context show` to verify the local transcript has the expected prior
   turn.
3. Run `context trace tail --pretty` after a bad route.
4. Check whether `conversation_context_sent=true`.
5. Check whether `pending=true` when a follow-up was meant to fill missing
   capability input.
6. If context was too thin, use `context privacy verbatim` temporarily to test
   whether exact wording changes the route.
7. Return to `context privacy digest` after debugging.

