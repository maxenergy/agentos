# G005 Capability Contract Validation Facade

Status: ready

## Objective

Define a single Capability Contract validation result shape and facade while preserving existing validation behavior.

## Context

Read `CONTEXT.md` terms:

- Capability Declaration
- Capability Contract
- Route Hint
- Policy Decision

## Scope

Allowed files:

- `src/core/schema/schema_validator.hpp`
- `src/core/schema/schema_validator.cpp`
- new `src/core/schema/capability_contract.*` if a separate facade is cleaner
- `tests/file_skill_policy_tests.cpp`
- `tests/spec_parsing_tests.cpp`

Out of scope:

- Moving all callers to the facade.
- Adding new JSON Schema keywords.
- Runtime authorization decisions.

## Requirements

- Introduce one result shape for declaration-time validation with:
  - validity;
  - error code;
  - human-readable message;
  - structured field/constraint diagnostics where practical.
- Preserve current public error strings for existing callers.
- Make clear that Capability Contract validation checks declarations and input/output shape; PolicyEngine still owns runtime authorization.

## Acceptance

- Existing schema, file skill, and spec parsing tests pass.
- New focused tests prove the facade can represent required-field, type, constraint, malformed schema, invalid risk, and unknown permission diagnostics.
- No runtime PolicyEngine behavior changes.

## Verification

```bash
cmake --build build --target agentos_file_skill_policy_tests agentos_spec_parsing_tests
ctest --test-dir build -R "agentos_file_skill_policy_tests|agentos_spec_parsing_tests" --output-on-failure
git diff --check
```

