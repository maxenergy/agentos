# AgentOS Context

AgentOS is a local AI agent operating-system kernel that routes tasks to managed skills, external tools, and agent adapters while preserving policy, audit, memory, and runtime-state discipline.

## Language

**Agent Dispatch**:
The act of choosing an Agent Adapter invocation path, applying policy, forwarding cancellation, invoking the agent, and producing a Dispatch Result suitable for task-step recording.
_Avoid_: agent execution, adapter execution, invoke path

**Dispatch Result**:
The outcome of an Agent Dispatch, including a task step candidate plus the agent output needed by callers for finalization, aggregation, or decomposition.
_Avoid_: step result, agent result

**Subagent Orchestration**:
The coordination of multiple agent task steps, including candidate selection, role assignment, optional decomposition, and aggregation.
_Avoid_: multi-agent execution, agent batch

**Capability Declaration**:
A code-defined or repo-local declaration of a Skill or Plugin capability that can be validated and loaded into AgentOS registries.
_Avoid_: spec file, tool config

**Route Hint**:
Descriptive metadata in a Capability Declaration that helps AgentOS decide when a capability is relevant.
_Avoid_: routing spec, skill routes file

**Interactive Intent**:
A REPL-local user intent that AgentOS can satisfy deterministically without Agent Dispatch, such as inspecting or updating the main chat model, listing skills, checking jobs, or reading runtime memory.
_Avoid_: chat command, small task, local route

**Route Tier**:
The runtime priority class used to choose between hard local handling, direct skill invocation, research, development, chat, and confirmation before any agent is invoked.
_Avoid_: route score, keyword priority, model choice

**Route Proposal**:
A structured candidate interpretation of a user request, including intent, slots, risk signals, and suggested Route Tier. A Route Proposal may come from deterministic parsing or a model, but it is not permission to execute.
_Avoid_: model decision, final route, classification result

**Route Verdict**:
The runtime-owned decision that accepts, rejects, or downgrades a Route Proposal after applying Route Tier rules, Capability Contracts, Policy Decisions, and slot completeness.
_Avoid_: model output, router guess

**Capability Contract**:
The input, output, permissions, risk, and Route Hint expectations that a Capability Declaration exposes to AgentOS before it can be invoked.
_Avoid_: schema, JSON schema, validator rules

**Capability Output**:
The structured output returned by an invoked capability after protocol-specific details have been normalized.
_Avoid_: plugin output, stdout payload

**Plugin Session**:
A reusable local plugin process owned by Plugin Host for persistent plugin protocol calls.
_Avoid_: resident capability session, agent session

**Runtime Store**:
AgentOS-owned local state that must be versioned, recoverable, inspectable, and portable across workspaces.
_Avoid_: storage backend, database, TSV files

**Audit History**:
Append-only runtime evidence of task, policy, trust, configuration, and storage events.
_Avoid_: log file, reconstructed state

**Auth Profile**:
A named provider-scoped selection of an auth session or credential source used when invoking provider-backed agents or auth commands.
_Avoid_: account, provider account, user account

**Auth Login Flow**:
The mode-specific process that creates or refreshes an Auth Profile session for a provider.
_Avoid_: credential store, token backend

**Credential Store**:
The platform-specific storage capability used to read, write, delete, or report token references.
_Avoid_: login flow, auth profile

**Policy Decision**:
A runtime decision about whether a specific task may invoke a capability or agent under the current identity, grants, risk, and approval state.
_Avoid_: contract validation, declaration validation

## Relationships

- **Agent Dispatch** happens after routing has selected an agent target.
- **Agent Dispatch** invokes exactly one Agent Adapter for one task step.
- **Agent Dispatch** produces one **Dispatch Result**.
- A **Dispatch Result** contains a task step candidate and may also carry structured agent output for callers.
- **Agent Dispatch** owns policy evaluation and policy audit for the dispatched agent.
- The caller of **Agent Dispatch** owns when the task step candidate is recorded.
- **Subagent Orchestration** uses **Agent Dispatch** for each selected agent and for the optional decomposition planner.
- **Subagent Orchestration** interprets decomposition output into subtask objectives.
- A **Capability Declaration** can produce a Skill or Plugin registration.
- A **Capability Declaration** may be code-defined for builtin skills or repo-local for learned CLI skills and plugins.
- A **Capability Declaration** carries **Route Hints** through its manifest metadata.
- An **Interactive Intent** is a hard local route candidate and wins over model-assisted routing when its slots are complete.
- A **Route Proposal** is advisory until AgentOS turns it into a **Route Verdict**.
- A **Route Verdict** owns the final Route Tier and may choose local handling, direct skill invocation, research, development, chat, or confirmation.
- A **Route Tier** that can write workspace files, use the network, or run long agent tasks must pass the relevant Capability Contract and Policy Decision before execution.
- A **Capability Declaration** exposes a **Capability Contract**.
- JSON Schema describes the structured input and output portion of a **Capability Contract**.
- **Capability Contract** validation decides whether a declaration is well-formed.
- An invoked capability returns **Capability Output**.
- A **Policy Decision** decides whether a specific invocation is allowed.
- A **Plugin Session** belongs to Plugin Host and is distinct from an agent workspace session.
- The **Runtime Store** contains auth sessions, memory, trust records, scheduler state, plugin host configuration, and audit history.
- **Audit History** is part of the **Runtime Store**, but it is append-only evidence rather than ordinary mutable state.
- Some **Audit History** entries can be reconstructed from memory and scheduler state as a recovery fallback.
- An **Auth Profile** is selected explicitly or through a provider default profile mapping.
- An **Auth Profile** is not an AgentOS user or a provider account identity.
- An **Auth Login Flow** can write token references through the **Credential Store**.
- The **Credential Store** owns backend selection and platform fallback behavior.

## Example dialogue

> **Dev:** "Should Router decide whether an agent uses V2 invoke or legacy run_task?"
> **Domain expert:** "No. Router selects the target; **Agent Dispatch** owns the invocation path."
>
> **Dev:** "Is the decomposition planner a special call path?"
> **Domain expert:** "No. The planner is dispatched like any other agent; **Subagent Orchestration** interprets the plan it returns."
>
> **Dev:** "Is learning a new CLI tool part of Agent Dispatch?"
> **Domain expert:** "No. It creates a **Capability Declaration**; dispatch only happens later when the loaded capability is invoked."
>
> **Dev:** "Do builtin file skills have contracts?"
> **Domain expert:** "Yes. Builtin skills are code-defined **Capability Declarations** and expose **Capability Contracts** like external capabilities."
>
> **Dev:** "Should we add a separate skill routes file?"
> **Domain expert:** "Not by default. **Route Hints** belong in the loaded **Capability Declaration** unless they need a separate lifecycle."
>
> **Dev:** "Is this just schema validation?"
> **Domain expert:** "No. JSON Schema is only part of the **Capability Contract**; risk, permissions, and Route Hints matter too."
>
> **Dev:** "If a permission is declared correctly, does that mean the task can run?"
> **Domain expert:** "No. The **Capability Contract** can be valid while the **Policy Decision** still denies this invocation."
>
> **Dev:** "Should callers parse plugin stdout?"
> **Domain expert:** "No. Plugin protocol details are normalized before callers see **Capability Output**."
>
> **Dev:** "Is an agent result a Capability Output?"
> **Domain expert:** "No. Agent calls produce **Dispatch Results**; Skills and Plugins produce **Capability Output**."
>
> **Dev:** "Should plugin process sessions and agent sessions share one abstraction?"
> **Domain expert:** "No. A **Plugin Session** is local process reuse; an agent workspace session is adapter conversation state."
>
> **Dev:** "Is TSV part of the domain language?"
> **Domain expert:** "No. TSV is one adapter for the **Runtime Store**; the domain concept is the local runtime state."
>
> **Dev:** "If audit can be rebuilt from memory, is it equivalent to original audit?"
> **Domain expert:** "No. Rebuild is a recovery fallback; **Audit History** is append-only evidence."
>
> **Dev:** "Should the login UI ask for an account?"
> **Domain expert:** "Use **Auth Profile**. Account is ambiguous across provider identity, OS user, and AgentOS user."
>
> **Dev:** "Does browser OAuth decide whether to use Keychain or Secret Service?"
> **Domain expert:** "No. The **Auth Login Flow** obtains tokens; the **Credential Store** owns platform storage."
>
> **Dev:** "Can Agent Dispatch just return a task step?"
> **Domain expert:** "No. A **Dispatch Result** includes the step and the structured agent output needed for aggregation or decomposition."
>
> **Dev:** "Does Agent Dispatch write the step to audit?"
> **Domain expert:** "No. It produces a step candidate; the caller records it at the correct lifecycle point."
>
> **Dev:** "Who records the policy decision?"
> **Domain expert:** "**Agent Dispatch** records policy audit because policy is part of deciding whether the adapter may be invoked."

## Flagged ambiguities

- "Agent execution" was used for both target selection and adapter invocation; resolved: use **Agent Dispatch** only for the post-routing invocation step.
- "Planner call" was used as if decomposition bypassed normal agent handling; resolved: decomposition uses **Agent Dispatch**, while plan interpretation belongs to **Subagent Orchestration**.
- "`learn_skill`" was grouped with Agent Dispatch because the REPL can trigger it; resolved: it creates a **Capability Declaration** and belongs with validation/reload work.
- "`runtime/skill_routes.tsv`" was treated as the default route-hint source; resolved: use **Route Hints** in **Capability Declarations** unless a separate lifecycle is proven necessary.
- "Schema validation" was used for the whole capability-facing contract; resolved: use **Capability Contract** for the domain concept and keep schema validation as implementation language.
- "Permission validation" was used for both declaration checks and runtime authorization; resolved: **Capability Contract** validation checks declarations, while **Policy Decision** handles invocation authorization.
- "`plugin_output`" was used as the caller-facing concept; resolved: use **Capability Output** for normalized capability results, with plugin protocol details kept behind the plugin seam.
- "Capability Output" was considered for agent results; resolved: agent calls produce **Dispatch Results**, while Skills and Plugins produce **Capability Output**.
- "Resident capability session" was considered for Plugin and Agent lifecycles; resolved: keep **Plugin Session** separate until another capability has the same lifecycle semantics.
- "StorageBackend" was used as domain language; resolved: use **Runtime Store** for the domain state and StorageBackend only for the implementation seam.
- "Audit log" was treated like ordinary storage state; resolved: **Audit History** is append-only evidence inside the **Runtime Store**, and reconstruction is not semantic equivalence.
- "Account" was used for credential selection; resolved: use **Auth Profile** for provider-scoped auth selection.
- "Auth Login Flow" was used as if it owned platform token storage; resolved: **Auth Login Flow** calls the **Credential Store**, but backend selection belongs to **Credential Store**.
- "TaskStepRecord" was treated as the Agent Dispatch return value; resolved: dispatch returns a **Dispatch Result** that includes a step candidate plus caller-facing agent output.
- "Recording the step" was grouped into **Agent Dispatch**; resolved: callers own recording time so orchestration can preserve ordering and visibility.
- "Policy audit" was grouped with caller lifecycle recording; resolved: **Agent Dispatch** owns policy evaluation and policy audit, while callers own route/task/step lifecycle recording.
