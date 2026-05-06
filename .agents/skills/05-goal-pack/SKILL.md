# goal-pack

## Purpose

Generate Codex `/goal` runnable package.

## Inputs

- `docs/goal/REQUIREMENTS.md`
- `docs/goal/DESIGN.md`
- `docs/goal/IMPACT.md`
- `docs/goal/TASKS.md`
- `docs/goal/ACCEPTANCE.md`

## Outputs

- `docs/goal/GOAL.md`
- `docs/goal/CODEX_START.md`

## CODEX_START.md Example

/goal Read docs/goal/GOAL.md and execute it exactly.

Follow all contracts strictly.
Work task by task.

## Rules

- Do not inline large goal content into `/goal`.
- Always reference files.
- Keep goal minimal, context in docs.
