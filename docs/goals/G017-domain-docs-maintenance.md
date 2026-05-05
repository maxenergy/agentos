# G017 Domain Docs Maintenance

Status: ready

## Objective

Keep domain language and architecture documentation synchronized as goals land.

## Context

Read:

- `CONTEXT.md`
- `improve-plan.md`
- `docs/goals/backlog.md`

## Scope

Allowed files:

- `CONTEXT.md`
- `improve-plan.md`
- `docs/goals/*`
- `docs/ARCH_ALIGNMENT.md`
- `docs/ARCHITECTURE.md`
- `docs/ROADMAP.md`
- README
- ADRs only when the ADR criteria are met

Out of scope:

- Code implementation.
- Cosmetic doc rewrites unrelated to goal status.

## Requirements

- Use canonical terms from `CONTEXT.md`.
- Update goal status when a goal is completed.
- Add decision-log entries for clarified boundaries.
- Create ADRs sparingly and only for durable trade-offs.
- Avoid reintroducing ambiguous terms:
  - agent execution;
  - plugin output as caller-facing language;
  - account for Auth Profile;
  - StorageBackend as domain language;
  - schema validation for the whole Capability Contract.

## Acceptance

- Documentation matches implemented behavior.
- No duplicate or contradictory terminology is introduced.
- Goal packet statuses are current.

## Verification

```bash
git diff --check
```

