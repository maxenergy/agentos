# G009 Plugin Protocol Output Normalization

Status: done
Depends on: G006

## Objective

Normalize Plugin protocol output before PluginSkillInvoker maps it to Skill-facing Capability Output.

## Context

Read `CONTEXT.md` terms:

- Capability Output
- Plugin Session
- Capability Contract

## Scope

Allowed files:

- `src/hosts/plugin/plugin_execution.cpp`
- `src/hosts/plugin/plugin_host.hpp`
- `src/hosts/plugin/plugin_json_rpc.*`
- `src/hosts/plugin/plugin_skill_invoker.cpp`
- `src/hosts/plugin/plugin_schema_validator.*`
- `tests/cli_plugin_tests.cpp`

Out of scope:

- Process-pool policy changes.
- Plugin Session admin UX.
- New plugin protocols.

## Requirements

- PluginRunResult carries protocol-normalized structured output or a serialized structured object.
- Protocol-specific stdout/JSON-RPC parsing is not done in PluginSkillInvoker.
- PluginSkillInvoker maps normalized protocol output into Capability Output.
- Existing output shape remains backward compatible unless tests are intentionally updated.

## Acceptance

- PluginSkillInvoker no longer parses plugin stdout.
- Protocol validation and output schema validation have locality before SkillResult mapping.
- Tests cover stdio JSON object output, JSON-RPC result object output, invalid output, and schema failure.

## Verification

```bash
cmake --build build --target agentos_cli_plugin_tests
ctest --test-dir build -R agentos_cli_plugin_tests --output-on-failure
git diff --check
```

Completed verification:

- `cmake --build build-codex-g014 --target agentos_cli_plugin_tests`
- `ctest --test-dir build-codex-g014 -R agentos_cli_plugin_tests --output-on-failure`
- `rg -n "stdout_text|JsonRpcResultObject|JsonRpcOutputError|structured_output_json" src/hosts/plugin/plugin_skill_invoker.cpp src/hosts/plugin/plugin_execution.cpp src/hosts/plugin/plugin_host.hpp tests/cli_plugin_tests.cpp`
- `git diff --check -- src/hosts/plugin/plugin_host.hpp src/hosts/plugin/plugin_execution.cpp src/hosts/plugin/plugin_skill_invoker.cpp tests/cli_plugin_tests.cpp docs/goals/G009-plugin-protocol-output.md docs/goals/backlog.md docs/goals/decision-log.md`

Note: repo-wide `git diff --check` currently reports pre-existing whitespace/line-ending diagnostics in unrelated modified files, so a focused diff check was used for this packet.
