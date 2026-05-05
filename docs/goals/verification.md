# Verification Commands

Use focused verification first, then broader verification when a goal touches shared behavior.

## Basic Checks

```bash
git diff --check
cmake --build build
ctest --test-dir build --output-on-failure
```

## Focused Test Targets

Agent Dispatch and Subagent Orchestration:

```bash
cmake --build build --target agentos_subagent_session_tests
ctest --test-dir build -R agentos_subagent_session_tests --output-on-failure
```

Capability Contract, Skill input, and Plugin output:

```bash
cmake --build build --target agentos_file_skill_policy_tests agentos_cli_plugin_tests agentos_spec_parsing_tests
ctest --test-dir build -R "agentos_file_skill_policy_tests|agentos_cli_plugin_tests|agentos_spec_parsing_tests" --output-on-failure
```

CLI, REPL, learned skills, and route hints:

```bash
cmake --build build --target agentos_cli_integration_tests agentos_learn_skill_tests agentos_main_agent_prompt_tests
ctest --test-dir build -R "agentos_cli_integration_tests|agentos_learn_skill_tests|agentos_main_agent_prompt_tests" --output-on-failure
```

Runtime Store and Audit History:

```bash
cmake --build build --target agentos_storage_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_storage_tests|agentos_cli_integration_tests" --output-on-failure
```

Auth Login Flow and Auth Profile behavior:

```bash
cmake --build build --target agentos_auth_tests agentos_agent_provider_tests agentos_cli_integration_tests
ctest --test-dir build -R "agentos_auth_tests|agentos_agent_provider_tests|agentos_cli_integration_tests" --output-on-failure
```

## When To Run Full Verification

Run the full build and CTest suite when a goal changes:

- `src/core/models.hpp`
- shared policy or permission code
- Agent Dispatch or Subagent Orchestration
- Capability Contract validation used by multiple hosts
- Runtime Store storage helpers
- CMake target registration

