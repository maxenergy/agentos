# spec-freeze

## Purpose

Freeze the current conversation into implementation-ready specs before Codex `/goal` runs.

## Inputs

- Existing conversation
- `docs/system/*` when working inside an existing project
- `docs/goal/QUESTIONS.md`
- `docs/goal/ANSWERS.md`
- `docs/goal/ASSUMPTIONS.md`
- `docs/goal/UNKNOWN.md`

## Outputs

Write or update:

- `docs/goal/REQUIREMENTS.md`
- `docs/goal/DESIGN.md`
- `docs/goal/NON_GOALS.md`
- `docs/goal/ACCEPTANCE.md`
- `docs/goal/DECISIONS.md`

## Required Sections

### REQUIREMENTS.md

- Problem
- Target users
- Use cases
- Functional requirements
- Non-functional requirements
- Constraints
- Open questions that block implementation

### DESIGN.md

- Proposed architecture
- Data flow
- Module responsibilities
- Public interfaces
- Error handling
- Security and privacy concerns

### NON_GOALS.md

- Explicitly excluded features
- Implementation styles to avoid
- Files or modules that must not be changed
- Scope creep traps

### ACCEPTANCE.md

- User-visible acceptance criteria
- Technical acceptance criteria
- Regression criteria
- Manual verification checklist

### DECISIONS.md

- User-confirmed decisions
- Agent-made assumptions
- Rejected alternatives

## Rules

- Do not implement code.
- Do not hide ambiguity.
- If a requirement is unclear but non-blocking, label it as an assumption.
- If a requirement is unclear and blocking, stop and ask the smallest possible question.
- Treat `NON_GOALS.md` as strongly as `REQUIREMENTS.md`.
