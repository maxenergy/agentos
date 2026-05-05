# Goal Backlog

Status values:

- `ready`: can be started now.
- `blocked`: depends on another goal or an external condition.
- `done`: completed and verified.

| Goal | Status | Depends On | Risk | Summary |
| --- | --- | --- | --- | --- |
| G001 | done | none | high | Create the Agent Dispatch seam and Dispatch Result type. |
| G002 | done | G001 | high | Move AgentLoop and SubagentManager agent paths onto Agent Dispatch. |
| G003 | ready | G002 | medium | Route decomposition planner dispatch through Agent Dispatch. |
| G004 | ready | G002 | medium | Wire REPL development/research dispatch through normal routing. |
| G005 | done | none | medium | Define the Capability Contract validation result and facade. |
| G006 | done | G005 | medium | Move Skill input and Plugin output callers onto Capability Contract validation. |
| G007 | ready | G005 | medium | Complete learned Capability Declaration validation and reload for `learn_skill`. |
| G008 | ready | G005 | medium | Replace route-hints TSV direction with Capability Declaration Route Hint coverage. |
| G009 | ready | G006 | high | Normalize Plugin protocol output before PluginSkillInvoker mapping. |
| G010 | blocked | G009 | medium | Clarify Plugin Session process-pool policy and admin scope. |
| G011 | done | none | high | Introduce the Runtime Store StorageBackend seam with a TSV adapter. |
| G012 | done | G011 | medium | Move storage commands and diagnostics through the StorageBackend seam. |
| G013 | ready | G011 | medium | Harden Audit History recovery tests and document lossy reconstruction. |
| G014 | done | none | medium | Extract Auth Login Flow mode modules from provider adapters. |
| G015 | ready | G014 | medium | Define Auth Profile selection across login modes. |
| G016 | blocked | external provider | low | Track Anthropic PKCE defaults and promote only when stable endpoints exist. |
| G017 | ready | none | low | Keep domain language and architecture docs synchronized during goals. |

## Recommended Execution Batches

Batch 1:

- G001
- G005
- G011
- G014

Batch 2:

- G002
- G006
- G012
- G015

Batch 3:

- G003
- G004
- G007
- G008
- G009
- G013

Batch 4:

- G010
- G016
- G017 as ongoing maintenance
