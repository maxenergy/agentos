# task-slice

## Purpose

Break requirements into independently verifiable tasks.

## Inputs

- `docs/goal/REQUIREMENTS.md`
- `docs/goal/DESIGN.md`
- `docs/goal/IMPACT.md`

## Output

- `docs/goal/TASKS.md`

## Task Template

Each task must include:

- Goal
- Scope
- Files likely touched
- Constraints
- Acceptance criteria
- Verification command
- Rollback condition
- Dependencies
- Do not touch

## Rules

- Tasks must be small enough to verify independently.
- Tasks must not overlap in scope.
- Each task must have a clear success/failure signal.
- Tasks must be ordered.
