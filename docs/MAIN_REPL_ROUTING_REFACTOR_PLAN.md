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

## Non-goals For This Batch

- Persist chat sessions across process restarts.
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

Latest result: all focused tests passed, full suite passed `24/24`, and
`git diff --check` passed.

## Next Candidate Slices

- Move REPL chat/pending-route state out of `interactive_commands.cpp` into a
  small module with direct unit tests. (done)
- Add an explicit approval path for high-risk main route actions instead of
  hard rejecting them. (done)
- Persist chat/session context across REPL restarts if the product needs
  long-running conversational continuity.
