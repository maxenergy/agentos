# impact-analysis

## Purpose

Determine the impact of a change on an existing system before implementation.

## Inputs

- `docs/system/*`
- `docs/goal/REQUIREMENTS.md`
- `docs/goal/DESIGN.md`

## Outputs

- `docs/goal/IMPACT.md`

## Required Sections

### Affected Modules

List all modules that will be changed.

### Affected Interfaces

List APIs, CLI commands, or UI flows impacted.

### Data Impact

- Schema changes
- Migrations required
- Backward compatibility

### Risk Assessment

- High-risk files
- Concurrency issues
- Performance risks

### Constraints

- Files that must not be modified
- Backward compatibility requirements

### Rollback Strategy

- How to revert safely

## Rules

- Be conservative: assume higher impact unless proven otherwise.
- Explicitly list unknown impact areas.
