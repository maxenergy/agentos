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

**AutoDev Job**:
An AgentOS-owned long-lifecycle software-engineering workflow that carries a request through clarification, system understanding, specification freeze, task slicing, code execution, verification, diff guarding, rollback decisions, final review, and PR-ready output.
_Avoid_: development request, coding prompt, Codex job

**AutoDev Job ID**:
The AgentOS-generated stable identifier for an AutoDev Job, formatted as `autodev-YYYYMMDD-HHMMSS-<6hex>` using a UTC timestamp and lowercase random hex suffix.
_Avoid_: task id, user-supplied id, runtime path

**AutoDev Job Directory**:
The job-scoped Runtime Store directory under `<agentos_workspace>/runtime/autodev/jobs/<job_id>/` containing current job facts, append-only events, and later task, turn, artifact, log, and spec-revision records.
_Avoid_: target repo runtime, job worktree, docs/goal

**Development Request**:
A lightweight interactive entry point for development-like user requests. It may be handled by a direct development capability or may create an AutoDev Job, but it does not own the AutoDev lifecycle.
_Avoid_: AutoDev job, implementation task, Codex run

**Development Complexity Gate**:
The development-request decision point that classifies development work as lightweight, AutoDev candidate, or AutoDev required, and recommends whether to run a legacy path, suggest AutoDev, request confirmation, or submit AutoDev.
_Avoid_: REPL classifier, route verdict, acceptance gate

**Explicit AutoDev Request**:
A user or UI request that directly asks to create or use AutoDev, such as an `autodev submit` command or natural language request to use the AutoDev workflow.
_Avoid_: inferred development complexity, normal development request

**AutoDev Submit Service**:
The single AgentOS service boundary that creates AutoDev Jobs and writes their initial Runtime Store records, regardless of whether the caller is CLI, REPL, scheduler, ravbot, or future UI.
_Avoid_: REPL job creation, command parser, development_request

**Codex CLI Development Skill**:
The transitional development execution capability that uses one-shot Codex CLI execution for a Development Request.
_Avoid_: AutoDev backend, AutoDev orchestrator, acceptance gate

**AutoDev Execution Adapter**:
The AutoDev-owned execution facade that runs or repairs one coding turn for a task while hiding whether the backend is transitional Codex CLI or a future persistent Codex app-server.
_Avoid_: development_request, AutoDev orchestrator, acceptance gate

**Codex CLI AutoDev Adapter**:
The transitional AutoDev Execution Adapter that uses Codex CLI with best-effort context continuity and synthetic lifecycle events.
_Avoid_: CodexAppServerAgent, persistent thread, production AutoDev executor

**Codex App Server Agent**:
The target AutoDev execution backend that provides persistent threads, native event streaming, same-thread repair, realtime diff and plan events, and app-server turn lifecycle integration.
_Avoid_: Codex CLI adapter, development_request, one-shot executor

**Continuity Mode**:
An execution-adapter declaration of conversational continuity, such as stateless turns, best-effort context, or persistent thread.
_Avoid_: session support, retry mode, repair status

**Event Stream Mode**:
An execution-adapter declaration of event fidelity, such as none, synthetic lifecycle events, or native provider events.
_Avoid_: logging, status, streaming output

**Acceptance Gate**:
The AgentOS-owned completion decision for an AutoDev task or job, based on verification evidence, acceptance criteria, diff policy, blocked files, retry limits, and final review state.
_Avoid_: Codex self-report, task summary, model completion

**AgentOS Workspace**:
The repository or workspace where AgentOS itself runs and owns its Runtime Store.
_Avoid_: target repo, job worktree, project workspace

**Target Repo Path**:
The user's original project repository path that an AutoDev Job is asked to change.
_Avoid_: AgentOS workspace, runtime store, job state directory

**Job Worktree Path**:
The isolated worktree created for one AutoDev Job to hold goal documents and code changes without polluting the Target Repo Path.
_Avoid_: runtime store, target repo, AgentOS workspace

**Goal Documents**:
The human- and model-readable engineering contract files for an AutoDev Job, usually under `docs/goal/` in the Job Worktree Path.
_Avoid_: runtime facts, audit state, acceptance source of truth

**Verification Evidence**:
AgentOS-owned structured results from verification commands, acceptance checks, diff guard, rollback, and final review that are used by the Acceptance Gate.
_Avoid_: verify summary, markdown report, Codex output

**Goal Report**:
An AgentOS-generated, human-readable markdown summary in the Job Worktree Path, such as `VERIFY.md` or `FINAL_REVIEW.md`.
_Avoid_: verification evidence, acceptance source of truth, runtime record

**Managed Goal Report**:
A Goal Report owned and regenerated by AgentOS, which Codex may read for context but cannot edit to change AutoDev facts.
_Avoid_: user-authored goal doc, code diff, acceptance record

**AutoDev Spec**:
The machine-readable runtime contract for an AutoDev Job, generated as `AUTODEV_SPEC.json` but only executable after AgentOS validates, normalizes, snapshots, and hashes it.
_Avoid_: prompt template, task markdown, skill output

**AutoDev Spec Snapshot**:
An immutable AgentOS Runtime Store revision of an AutoDev Spec, including normalized content, hash, schema version, source skill-pack metadata, and approval or resnapshot provenance.
_Avoid_: worktree spec file, current AUTODEV_SPEC.json, latest task list

**AutoDev Skill Pack Binding**:
The AutoDev Job's declared or loaded relationship to a skills protocol pack, including source, path or ref, load status, compatibility, and manifest hash when available.
_Avoid_: loaded pipeline, spec provenance, skill registry entry

**Skill Pack Snapshot**:
The AgentOS Runtime Store metadata capturing the exact skill pack facts used by a docs pipeline or spec revision, such as commit, manifest hash, required steps, and supported schema versions.
_Avoid_: declared skill pack path, current checkout, job binding

**Candidate Spec**:
The unfrozen set of goal documents and candidate AutoDev Spec generated by the docs pipeline in the Job Worktree Path.
_Avoid_: frozen spec, approved contract, execution source of truth

**Frozen Spec**:
An AgentOS-approved AutoDev Spec Snapshot revision that is allowed to drive code execution and Acceptance Gate decisions.
_Avoid_: generated spec, worktree AUTODEV_SPEC.json, model-approved plan

**Spec Freeze**:
The AgentOS-controlled action that validates, normalizes, snapshots, hashes, and approves a Candidate Spec into a Frozen Spec.
_Avoid_: docs generation, model declaration, skill completion

**Autonomy Level**:
The configured degree of AutoDev self-direction, with supervised mode requiring human approval before code execution.
_Avoid_: approval gate, risk level, execution mode

**AutoDev Status**:
The lifecycle and continuability state of an AutoDev Job or task, answering whether it can proceed, is paused, or has reached a terminal outcome.
_Avoid_: workflow step, current operation, progress percentage

**AutoDev Phase**:
The major workflow stage of an AutoDev Job, such as workspace preparation, system understanding, goal packing, code execution, final review, or PR preparation.
_Avoid_: status, approval wait, temporary activity

**AutoDev Current Activity**:
The short-lived operation currently being performed by an AutoDev Job or task, such as creating a worktree, validating a spec, running a Codex turn, verifying, diff guarding, repairing, or rolling back.
_Avoid_: status, phase, completion evidence

**Approval Gate**:
The explicit approval point currently blocking an AutoDev Job, such as approval before workspace preparation, code execution, risky rollback, spec revision, or PR creation.
_Avoid_: phase, blocker reason, policy decision

**Workspace Preparation**:
The AutoDev phase that creates or validates the Job Worktree Path and proves workspace isolation is ready before any target-project writes occur.
_Avoid_: submit, checkout, setup script

**Planned Worktree Path**:
The worktree path calculated and recorded by AutoDev submit before workspace preparation has created or reserved it.
_Avoid_: ready worktree, actual worktree, prepared workspace

**Isolation Mode**:
The AutoDev workspace strategy for a job, either an isolated git worktree or an explicit in-place compatibility mode.
_Avoid_: sandbox mode, workspace path, checkout type

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
- An **AutoDev Job** is a top-level workflow subsystem inside AgentOS Runtime, not a renamed **Development Request**.
- An **AutoDev Job ID** is generated by AgentOS, not by users, models, Codex, or skills.
- AutoDev Job IDs must match `^autodev-[0-9]{8}-[0-9]{6}-[0-9a-f]{6}$` before being used in Runtime Store paths.
- AutoDev Job IDs use UTC timestamps for stable lexical time ordering; human-facing local time may be stored separately.
- Job runtime state lives under the **AutoDev Job Directory**.
- MVP 1 creates only `job.json` and `events.ndjson` in the AutoDev Job Directory.
- `job.json` is the current fact snapshot.
- `events.ndjson` is append-only history, with one JSON object per line.
- MVP 1 does not require global job indexes; status by job id reads the AutoDev Job Directory directly.
- Missing job directories or missing `job.json` files produce explicit not-found errors and must not trigger target-repo or filesystem-wide searches.
- Target repos and job worktrees must not contain AgentOS AutoDev runtime job facts.
- A **Development Request** can create or forward to an **AutoDev Job**, but it does not own AutoDev state, progress, rollback, or completion.
- MVP AutoDev creates jobs only from an **Explicit AutoDev Request** or an explicit user confirmation after a suggestion.
- MVP 1 creates AutoDev Jobs only through top-level CLI commands such as `agentos autodev submit`.
- MVP 1 AutoDev status is read through top-level CLI commands such as `agentos autodev status`.
- The interactive REPL must not create AutoDev Jobs in MVP 1.
- In MVP 1, the REPL may recognize an Explicit AutoDev Request but should only print CLI guidance.
- Route Verdict may reserve an explicit AutoDev route tier, but MVP 1 uses a guidance action rather than a submit action in the REPL.
- REPL must not guess the Target Repo Path for AutoDev submit in MVP 1.
- REPL must not start AutoDev confirmation, approval, workspace preparation, docs generation, or Codex execution flows in MVP 1.
- Future REPL, scheduler, ravbot, and UI entrypoints must call the same **AutoDev Submit Service** instead of duplicating job creation logic.
- Regular Development Requests continue to use the lightweight legacy development path in MVP.
- A Development Request may run a **Development Complexity Gate** and classify work as `lightweight`, `autodev_candidate`, or `autodev_required`.
- In MVP, inferred `autodev_candidate` and `autodev_required` results produce suggestions or confirmation prompts, not silent AutoDev job creation.
- `lightweight` development work is narrow, clearly scoped, and does not require specification freeze, tests, rollback, PR-ready output, or long-running progress tracking.
- `autodev_candidate` development work likely benefits from AutoDev but does not explicitly require it.
- `autodev_required` development work includes explicit AutoDev requests, PR-ready requests, full closed-loop execution requests, multi-step debug or refactor work, risky changes, rollback or diff-guard requests, or requests requiring specification and acceptance gates.
- The REPL classifier identifies broad Route Tiers; development complexity belongs inside Development Request handling.
- A Development Request can recommend or submit AutoDev, but once an AutoDev Job exists it cannot bypass workspace isolation, Spec Freeze, approval gates, verification, diff guard, rollback, or Acceptance Gate.
- AutoDev execution is never entered without an explicit submit or confirmation plus the normal workspace and spec-freeze gates.
- MVP AutoDev submit does not load or validate skill packs.
- AutoDevJob still includes an **AutoDev Skill Pack Binding** from the first version.
- Submit may record a skill pack name, local path, source URI, ref, or version, but in MVP it records only `not_loaded` or `declared` binding status.
- SkillPackLoader begins after MVP submit/status and is responsible for loading, validating, compatibility checks, required AutoDev step discovery, and manifest hashing.
- The docs pipeline cannot run until the AutoDev Skill Pack Binding is loaded and required AutoDev steps are validated.
- Loading a skill pack is a runtime-only AutoDev activity that may write a Skill Pack Snapshot under the AutoDev Job Directory but must not generate Goal Documents or touch the Target Repo Path.
- A job's AutoDev Skill Pack Binding and an AutoDev Spec's generation provenance are related but distinct.
- Each AutoDev Spec Snapshot records the **Skill Pack Snapshot** used to generate that spec revision, not merely the job's declared binding.
- Loading a skill pack never implies that a Candidate Spec exists, that a Spec Freeze happened, or that code execution is approved.
- Generating goal-document skeletons writes Candidate Spec working files under the Job Worktree Path only, and does not validate, snapshot, freeze, approve, or execute the spec.
- The **Codex CLI Development Skill** is a legacy or MVP execution adapter for lightweight Development Requests and does not own AutoDev lifecycle concerns.
- AutoDev must reuse AgentOS routing, policy, audit, memory, runtime-store, agent-dispatch, workspace-session, subagent-orchestration, and scheduler concepts instead of defining a parallel operating system.
- Only an **Acceptance Gate** can mark an AutoDev task or job complete.
- Codex turn completion or self-reported completion is evidence for AutoDev, but never sufficient to mark an AutoDev task or job done.
- AutoDevOrchestrator calls an **AutoDev Execution Adapter**, not `development_request` or a specific Codex CLI skill.
- The **Codex CLI AutoDev Adapter** may be used for MVP task execution, but it is transitional and must be marked with best-effort continuity and synthetic events.
- The **Codex App Server Agent** is the long-term execution backend for persistent-thread and native-event AutoDev execution.
- AutoDev execution adapters return execution turn results; they do not own job status, task status, acceptance status, rollback decisions, or final completion.
- AutoDev turn records must include adapter kind, adapter name, session id, provider thread or turn ids when available, **Continuity Mode**, and **Event Stream Mode**.
- Codex CLI execution uses `continuity_mode=best_effort_context` and `event_stream_mode=synthetic`.
- Codex app-server execution uses `continuity_mode=persistent_thread` and `event_stream_mode=native`.
- Synthetic events must not pretend to be native Codex app-server events such as plan updates, diff updates, token usage, or command-output deltas.
- Repair through the Codex CLI adapter is context-equivalent repair, not same-thread repair.
- Acceptance Gate is adapter-agnostic and evaluates only AgentOS Runtime Store facts.
- Codex CLI completion and Codex app-server `turn/completed` are execution evidence, not task completion.
- All AutoDev execution adapters run in the **Job Worktree Path** for coding, verification, diffing, and repair context.
- Write-enabled AutoDev execution requires a Frozen Spec, approved code-execution gate, policy permission, and ready workspace isolation.
- AutoDev fact state belongs in the **AgentOS Workspace** Runtime Store, under a job-scoped path such as `runtime/autodev/<job_id>/`.
- AutoDev fact state does not belong in the **Target Repo Path** or the **Job Worktree Path**.
- AutoDevStateStore roots must be derived from the **AgentOS Workspace**, not from a task workspace, Target Repo Path, or Job Worktree Path.
- A **Job Worktree Path** may contain **Goal Documents** and code changes because Codex and humans need a shared execution contract.
- **Goal Documents** are working artifacts by default; whether they are included in a final PR is controlled by delivery policy, not by their existence in the worktree.
- `commit_goal_docs=false` should be the default delivery policy unless the user explicitly wants process documents in the PR.
- `docs/goal/AUTODEV_SPEC.json` can exist in the **Job Worktree Path** as a machine-readable contract for Codex, but AgentOS must keep an immutable snapshot and hash in its Runtime Store.
- Codex may read Goal Documents and modify code in the Job Worktree Path, but must not modify AutoDev fact state in the AgentOS Runtime Store.
- `VERIFY.md` and `FINAL_REVIEW.md` in Goal Documents are human-readable summaries; structured verification and final-review facts in the Runtime Store remain the source of truth for the **Acceptance Gate**.
- AutoDev's default **Isolation Mode** is `git_worktree`.
- AutoDev submit may create only the job record, but **Workspace Preparation** must succeed and produce a valid **Job Worktree Path** before any step writes Goal Documents, runs Codex, verifies, computes diffs, rolls back, or modifies code.
- MVP 1 AutoDev submit creates only the AutoDev Job Runtime Store record.
- MVP 1 submit computes and records a **Planned Worktree Path**, but it does not create a git worktree, write goal documents, load skill packs, run Codex, verify, diff, or modify the Target Repo Path.
- MVP 1 submit defaults `isolation_mode=git_worktree`, `isolation_status=pending`, and `next_action=prepare_workspace`.
- MVP 1 submit must not claim workspace readiness.
- `prepare_workspace` is the first AutoDev action allowed to create a worktree and mark workspace isolation ready.
- AutoDev must not silently downgrade from `git_worktree` to `in_place`; failed worktree creation blocks the job unless the user explicitly chooses in-place mode.
- `in_place` is an explicit compatibility and debugging mode, not the default production path.
- In-place code-writing requires stronger approval and dirty-worktree checks than git-worktree execution.
- The **Target Repo Path** is read-only by default: it is used to derive repository metadata, base revision, remote information, and worktree creation inputs.
- Goal Documents are written to the **Job Worktree Path**, not to the original Target Repo Path.
- Verify commands, diff guard, rollback, and Codex execution operate on the **Job Worktree Path**, not on the Target Repo Path.
- Any project-writing phase must require `isolation_status=ready` and a valid Job Worktree Path, except explicitly approved in-place compatibility mode.
- AutoDev records the source revision used to create the worktree, such as `created_from_head_sha`, so completion and PR preparation are tied to a known base.
- A dirty Target Repo Path blocks default worktree preparation unless the user explicitly allows it, in which case AutoDev records that the target was dirty at submit.
- AgentOS owns the authoritative schema and runtime semantics for an **AutoDev Spec**.
- Skills own AutoDev Spec templates, examples, generation procedures, and clarification protocols, but cannot unilaterally change AgentOS execution semantics.
- An **AutoDev Spec** must declare its schema version, generating skill pack, skill-pack version or commit, minimum AgentOS version, objective, mode, source-of-truth documents, and tasks.
- AgentOS must fail closed for AutoDev Spec validation: missing schema version, unsupported schema version, invalid fields, missing required fields, or unknown execution semantics block execution.
- Unknown AutoDev Spec metadata may be preserved only when the schema declares it non-executable; unknown fields that affect scheduling, verification, diff policy, rollback, or acceptance must block execution.
- The worktree `docs/goal/AUTODEV_SPEC.json` is a candidate contract for humans, Codex, and skills; it is not the source of truth for execution.
- Before execution, AgentOS validates, normalizes, snapshots, and hashes the worktree AutoDev Spec into the Runtime Store.
- Acceptance Gate, verification, diff guard, rollback, and task scheduling use the **AutoDev Spec Snapshot**, not the mutable worktree `AUTODEV_SPEC.json`.
- Changing an AutoDev Spec after snapshot requires an explicit revalidate and resnapshot flow that records a new spec revision, previous hash, reason, and approval provenance.
- The docs pipeline produces a **Candidate Spec** only.
- **Spec Freeze** is an AgentOS action, not a model, Codex, or skills action.
- A **Frozen Spec** must be tied to a specific AutoDev Spec Snapshot hash and approval record.
- By default, `require_human_approval_before_code=true` and AutoDev runs with `autonomy_level=supervised`.
- MVP AutoDev implements supervised mode only.
- Code execution is forbidden until workspace isolation is ready and an approved **Frozen Spec** exists.
- Before code execution, AgentOS must validate and normalize the Candidate Spec, create a pending snapshot and hash, present the relevant summary and hash for approval, then mark the approved revision frozen.
- Approval before code execution approves a specific spec revision hash, not the mutable worktree goal documents.
- Blocking unknowns prevent Spec Freeze unless the user explicitly resolves them or approves downgrading them to assumptions.
- In supervised mode, AutoDev may automatically prepare the workspace, generate candidate goal documents, validate schema, and create a pending spec snapshot, but it must stop at the before-code approval gate.
- After a Spec Freeze, changing execution semantics requires a new spec revision, validation, snapshot, hash, approval, and revision-change event.
- Codex prompts during code execution must refer to the active Frozen Spec and must state that Codex cannot alter the spec to pass validation or self-certify completion.
- AgentOS writes structured verification and final-review facts to the Runtime Store as **Verification Evidence**.
- AgentOS may also write **Managed Goal Reports** such as `docs/goal/VERIFY.md` and `docs/goal/FINAL_REVIEW.md` into the Job Worktree Path.
- **Goal Reports** are summary-only and are not Acceptance Gate authority.
- Acceptance Gate uses Runtime Store verification records, final-review records, diff-guard records, acceptance records, and the active Frozen Spec, not markdown Goal Reports.
- Codex may read `VERIFY.md` for repair context, but editing it cannot change task status, job status, or Acceptance Gate results.
- AgentOS owns and may overwrite Managed Goal Reports.
- If Codex modifies a Managed Goal Report, Diff Guard records the managed-report modification; strict policies may treat it as a violation, while default policies ignore it for acceptance and let AgentOS regenerate the report.
- AutoDev defaults to `write_goal_reports=true`, `commit_goal_docs=false`, and `commit_goal_reports=false`.
- Final PR or patch output must not include generated Goal Reports unless delivery policy or the user explicitly requests them.
- Verification logs written to Goal Reports should be redacted and truncated; full raw evidence belongs in the Runtime Store artifacts.
- For Codex, `VERIFY.md` is repair context; for AgentOS, structured verification facts are authority.
- AutoDev separates state into **AutoDev Status**, **AutoDev Phase**, and **AutoDev Current Activity**.
- **AutoDev Status** must not encode every workflow step; it records lifecycle and continuability only.
- **AutoDev Phase** must not encode temporary actions or approval waits; approval waits are represented by status plus an **Approval Gate**.
- **AutoDev Current Activity** must not be used as completion evidence or progress source.
- AutoDev history is recorded in job-scoped events, while job and task records store only current fact state.
- If an AutoDev Job status is not `running`, its current activity is normally `none`.
- If an AutoDev Job status is `awaiting_approval`, it must identify a non-empty **Approval Gate**.
- If an AutoDev Job status is `blocked`, it must record a blocker.
- A job in code-execution phase normally has a current task unless all required tasks have passed and the job is moving to final review.
- A task may be marked passed only by the **Acceptance Gate**.
- A job may be marked done only after required tasks and final review pass under the **Acceptance Gate**.
- Crash recovery must treat stale current activities as recoverable observations, not proof that an operation is still running.
- Progress is computed from phase completion, task completion, acceptance records, and final-review records, not from current activity.

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
- "`development_request`" was treated as if it could become the AutoDev subsystem; resolved: **AutoDev Job** is a top-level workflow, while **Development Request** remains a lightweight entry point that may forward to AutoDev.
- "Complex development" was considered something the REPL classifier should fully decide; resolved: coarse routing belongs to Route Verdict, while **Development Complexity Gate** owns lightweight versus AutoDev recommendation decisions.
- "AutoDev required" was considered as automatic job creation; resolved: MVP uses explicit AutoDev triggers or user confirmation, not silent upgrade.
- "REPL AutoDev request" was considered as direct job creation; resolved: MVP 1 REPL only prints CLI guidance, while top-level CLI uses the **AutoDev Submit Service**.
- "AutoDev submit" was considered as command-specific logic; resolved: all future entrypoints must share the same **AutoDev Submit Service**.
- "AutoDev job id" was considered similar to short dev/research ids; resolved: AutoDev uses stable **AutoDev Job ID** format with path validation.
- "AutoDev runtime layout" was considered directly under `runtime/autodev/<job_id>`; resolved: job facts live under `runtime/autodev/jobs/<job_id>/` so global config, indexes, templates, and metrics can coexist later.
- "Submit" was considered the right time to load skills; resolved: MVP submit records an **AutoDev Skill Pack Binding**, while SkillPackLoader is introduced later.
- "Job skill pack" was conflated with spec provenance; resolved: **AutoDev Skill Pack Binding** records the job's declared or loaded pack, while **Skill Pack Snapshot** records the exact pack facts used for a spec revision.
- "Codex completed" was treated as completion evidence; resolved: Codex completion is only a signal, while **Acceptance Gate** owns task and job completion decisions.
- "`development_request`" was considered as the AutoDev executor; resolved: AutoDev execution goes through an **AutoDev Execution Adapter** abstraction.
- "Codex CLI repair" was considered equivalent to same-thread repair; resolved: CLI repair is best-effort context continuity, while same-thread repair requires the **Codex App Server Agent**.
- "Execution adapter result" was considered enough to pass a task; resolved: adapters produce turn evidence only, while Acceptance Gate decides pass/fail.
- "`runtime/autodev`" was considered inside the target project; resolved: AutoDev fact state belongs to the **AgentOS Workspace** Runtime Store, while the **Job Worktree Path** holds only Goal Documents and code changes.
- "`VERIFY.md`" was considered as possible completion evidence; resolved: markdown reports are summaries, while **Verification Evidence** in the Runtime Store is the source of truth.
- "Submit" was treated as if it implied a writable target workspace; resolved: **Workspace Preparation** is the hard gate before any target-project write.
- "Submit" was treated as if it creates the worktree; resolved: MVP 1 submit records a **Planned Worktree Path** only, while `prepare_workspace` performs worktree creation later.
- "`in_place`" was considered as an automatic fallback for worktree failures; resolved: AutoDev blocks instead of silently downgrading unless the user explicitly chooses in-place mode.
- "`AUTODEV_SPEC.json`" was considered a skills-owned template; resolved: skills generate a candidate **AutoDev Spec**, while AgentOS owns the schema, runtime semantics, validation, normalization, snapshots, and execution interpretation.
- "Latest worktree spec" was considered as an execution source; resolved: the **AutoDev Spec Snapshot** in the AgentOS Runtime Store is the execution source of truth.
- "AutoDev status" was used for phases, operations, approval waits, and terminal outcomes; resolved: use **AutoDev Status**, **AutoDev Phase**, **AutoDev Current Activity**, and **Approval Gate** as separate concepts.
- "Verifying" was considered as a status; resolved: verifying is an **AutoDev Current Activity** that can occur in multiple phases.
- "Generated spec" was considered equivalent to an approved contract; resolved: generated documents are a **Candidate Spec** until AgentOS performs **Spec Freeze**.
- "Freeze" was treated as something skills or Codex could declare; resolved: only AgentOS can create a **Frozen Spec** by validating, snapshotting, hashing, and approving a Candidate Spec.
- "`VERIFY.md`" and "`FINAL_REVIEW.md`" were considered possible completion authorities; resolved: they are **Goal Reports**, while Runtime Store **Verification Evidence** and final-review records remain authoritative.
- "Codex repair context" was conflated with AgentOS acceptance authority; resolved: Codex may read Goal Reports, but Acceptance Gate ignores markdown reports for completion decisions.
