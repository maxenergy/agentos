# G006 Move Callers To Capability Contract Validation

Status: blocked
Depends on: G005

## Objective

Move Skill input, Plugin output, CLI spec, and Plugin manifest validation callers onto the shared Capability Contract validation facade.

## Context

Read `CONTEXT.md` terms:

- Capability Contract
- Capability Declaration
- Capability Output
- Policy Decision

## Scope

Allowed files:

- `src/core/loop/agent_loop.cpp`
- `src/core/schema/*`
- `src/hosts/cli/cli_spec_loader.cpp`
- `src/hosts/plugin/plugin_manifest_loader.cpp`
- `src/hosts/plugin/plugin_schema_validator.*`
- `src/hosts/plugin/plugin_execution.cpp`
- `tests/file_skill_policy_tests.cpp`
- `tests/cli_plugin_tests.cpp`
- `tests/spec_parsing_tests.cpp`

Out of scope:

- Learned skill generation.
- Plugin protocol output normalization.
- New schema keywords.

## Requirements

- AgentLoop should not choose keyword-specific validators directly.
- PluginHost should not choose keyword-specific validators directly.
- CLI and Plugin loaders should validate declaration-time risk/permission/schema shape through the shared facade or an adapter around it.
- Preserve existing CLI-visible diagnostic text unless a test is deliberately updated.

## Acceptance

- Existing validation tests pass.
- Call sites use the facade instead of granular keyword-specific helpers.
- Runtime authorization remains in PolicyEngine.

## Verification

```bash
cmake --build build --target agentos_file_skill_policy_tests agentos_cli_plugin_tests agentos_spec_parsing_tests
ctest --test-dir build -R "agentos_file_skill_policy_tests|agentos_cli_plugin_tests|agentos_spec_parsing_tests" --output-on-failure
git diff --check
```

