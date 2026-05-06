# G017 Domain Docs Maintenance

Status: done

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

Completed verification on 2026-05-06:

```bash
rg -n 'agent execution|plugin output as caller-facing|successful plugin stdout|structured `plugin_output`|Multi-account|process-pool hardening|runtime session admin UX 待完善|deeper process-pool policy|Plugin Host deeper process-pool|Storage still lacks|learn_skill.*planned|REPL dispatch.*deferred|Route-hint consumption is missing' CONTEXT.md improve-plan.md docs/ARCH_ALIGNMENT.md docs/ARCHITECTURE.md docs/ROADMAP.md README.md docs/goals
git diff --check -- CONTEXT.md improve-plan.md docs/ARCH_ALIGNMENT.md docs/ARCHITECTURE.md docs/ROADMAP.md README.md docs/goals/G017-domain-docs-maintenance.md docs/goals/backlog.md docs/goals/decision-log.md
```

The terminology scan only reports intentional avoid-list examples in `CONTEXT.md` and this goal packet.
Focused `git diff --check` was used because repo-wide diff-check still reports pre-existing unrelated line-ending and whitespace diagnostics outside this goal's touched files.
