# Main REPL Routing Refactor Plan

## Goal

Make the interactive REPL behave like a conversational orchestrator: ordinary
natural language goes to the configured `main` agent with recent context, and
`main` decides whether to answer directly or request a tool/agent call.

## Problems

- The REPL currently performs final routing with local regexes before `main`
  sees the turn.
- Follow-up turns can be misrouted to `development_request` or
  `research_request` because the current line contains words such as
  "generate", "search", or Chinese equivalents.
- The `main` adapter is mostly stateless from the REPL's point of view, so it
  cannot reliably connect short follow-ups to prior turns.
- Agent/skill routing policy should come from live capability descriptions and
  the main agent's reasoning, not from a growing C++ phrase table.

## Target Architecture

1. REPL command layer handles only hard local commands:
   `help`, `jobs`, `run`, `memory`, `schedule`, explicit `chat`, and similar
   CLI-shaped operations.
2. Free-form natural language defaults to `main`.
3. The local classifier becomes a hard-local intent detector plus a light hint,
   not the final owner of research/development routing.
4. `main` receives:
   - live registered skills and agents from manifests/profiles;
   - recent REPL chat context;
   - context-first routing guidance that treats follow-ups, clarifications,
     and constraint updates as the same conversation before considering
     delegation;
   - a small routing contract that says it may answer directly or emit a
     structured route action.
5. A structured route action is a JSON object:

   ```json
   {
     "agentos_route_action": {
       "action": "call_capability",
       "target_kind": "skill",
       "target": "news_search",
       "brief": "GOAL: ... FORMAT: ... SUCCESS: ...",
       "mode": "sync",
       "arguments": {
         "query": "..."
       }
     }
   }
   ```

6. The REPL parses route actions, executes the requested capability through the
   normal AgentOS loop, then sends a compact tool-result turn back to `main` for
   synthesis.

## Slices

1. Main-first routing: done
   - Natural language no longer routes directly to development/research.
   - Hard local intents still stay local.
   - Existing explicit CLI commands continue to work.

2. Main context: done
   - Keep recent REPL chat turns in memory for the session.
   - Pass the compact recent context into `main` as runtime context.

3. Structured route-action skeleton: done
   - Teach `main` prompt the JSON route action contract.
   - Parse `agentos_route_action`.
   - Execute `call_capability` for registered skills/agents through the normal
     AgentOS loop.
   - Feed compact results back to `main` for final response.

4. Registry/schema validation: done
   - Validate route action shape before execution.
   - Reject unregistered targets.
   - Validate required skill inputs from `input_schema_json`.
   - Reject high-risk route actions unless a future explicit approval path is
     added.

5. Missing-input follow-up loop: done
   - Convert `InvalidRouteSkillInput` into a clarification-oriented result
     prompt.
   - Keep internal tool-result prompts out of chat history.
   - Store a pending route action for missing-input failures and pass that
     pending context to `main` on the next turn.
   - Clear pending state once the next route action no longer fails for missing
     inputs.

6. High-risk approval guidance: done
   - High-risk route actions without approval arguments return
     `ApprovalRequired` instead of executing.
   - The result includes the existing `agentos trust approval-request` path and
     tells `main` to explain the approval/retry flow.
   - Route actions carrying `allow_high_risk=true` and `approval_id=<id>` pass
     REPL validation and are left to the normal PolicyEngine approval check.

7. Tests and docs: done
   - Replace regex-final-routing expectations with main-first expectations.
   - Add prompt/action parsing regressions.
   - Update REPL dispatch docs.

8. Context-first continuation guard: done
   - Make `main` decide whether the live turn continues the prior topic before
     it emits any route action.
   - Pass a contextual REPL intent hint alongside recent transcript.
   - Cover the browser/low-frequency continuation scenario with a real
     interactive main-agent fixture.

9. Provider-native main-agent message history: done
   - Split main-agent chat requests into provider-native system/user/assistant
     messages instead of sending one large user prompt.
   - Parse recent REPL transcript turns into message roles before calling
     OpenAI-compatible and Anthropic-compatible providers.
   - Send Gemini/Vertex requests with `systemInstruction` plus user/model
     contents so the same context boundary is preserved there.

10. Persisted REPL main-agent context: done
   - Store recent main-agent REPL turns under
     `runtime/main_agent/sessions/repl-default.json`.
   - Reload that transcript when `agentos interactive` starts so REPL restarts
     keep conversational continuity.
   - Keep the persisted transcript capped to the same recent-turn window used
     for runtime context.

11. REPL context management commands: done
   - Add `context show` to inspect the current persisted main-agent REPL
     transcript and file path.
   - Add `context clear` to clear in-memory context, pending route state, and
     the persisted `repl-default.json` file.
   - Show `main_context_turns` in `status`.

12. Named REPL main-agent contexts: done
   - Add `context use <name>` to switch between separate persisted
     main-agent REPL transcripts.
   - Add `context list` to show available contexts, active context, and turn
     counts.
   - Persist the selected context name in
     `runtime/main_agent/current_context.txt` so REPL restarts resume the same
     topic.
   - Show the active `main_context` name in `status`.

13. Sanitized REPL context digest: done
   - Keep the full transcript local for `context show` and persisted context
     files.
   - Send `main` a compact `[REPL CONTEXT DIGEST]` instead of replaying the
     full recent transcript as provider-native prior messages.
   - Redact URLs, email-like values, opaque IDs, long numbers, and common
     automation-risk phrases from the digest before the external model sees
     the context.
   - Preserve context-first routing guidance so follow-up turns still stay in
     the main conversational path unless a live tool/action is needed.

14. Configurable REPL context privacy: done
   - Add `context privacy [digest|none|verbatim]`.
   - Default each named context to `digest`.
   - Persist per-context privacy under `runtime/main_agent/privacy/<name>.txt`.
   - Keep `none` available for strict privacy and `verbatim` available for
     local debugging when exact prior turns are needed.
   - Show active `main_context_privacy` in `status`.

15. Main routing trace: done
   - Write privacy-safe JSONL trace records to
     `runtime/main_agent/routing_trace.jsonl`.
   - Record main request metadata without user prompt text:
     `context_privacy`, whether conversation context was sent, whether a
     pending route action was sent, and whether route actions were allowed.
   - Record main responses, requested route-action targets, route-action
     execution results, and pending-action state transitions.
   - Show the trace path in `status` so misrouting can be debugged without
     guessing which prompt shape was used.

16. REPL trace inspection commands: done
   - Add `context trace tail [n]` to print recent routing trace JSONL records
     from inside the REPL.
   - Cap tail output at 100 records to avoid dumping large trace files.
   - Add `context trace clear` to truncate the routing trace file.

## Non-goals For This Batch

- Build a full multi-step planner loop.
- Remove explicit commands such as `run`, `schedule`, or `memory`.
- Auto-run destructive tool actions without existing AgentOS policy checks.

## Current Verification

```bash
ctest --test-dir build --output-on-failure -R "agentos_cli_integration_tests|agentos_main_route_action_tests|agentos_main_agent_prompt_tests|agentos_intent_classifier_tests|agentos_routing_eval_tests"
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

Latest result: all focused tests passed, full suite passed `25/25`, and
`git diff --check` passed.

## Next Candidate Slices

- Move REPL chat/pending-route state out of `interactive_commands.cpp` into a
  small module with direct unit tests. (done)
- Add an explicit approval path for high-risk main route actions instead of
  hard rejecting them. (done)
- Add context rename/delete/export commands if named contexts need lifecycle
  management beyond `use`, `list`, `show`, and `clear`.
- Add configurable context privacy levels if users need to choose between
  `digest`, `verbatim`, and `none` per REPL context. (done)
- Add a `context trace [tail|clear]` command for inspecting or resetting
  `routing_trace.jsonl` from inside the REPL. (done)
- Add a compact human-readable trace formatter if raw JSONL is too noisy for
  day-to-day debugging.
