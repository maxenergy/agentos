cmake_minimum_required(VERSION 3.20)

project(agentos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(agentos_core
    src/cli/agents_commands.cpp
    src/cli/auth_commands.cpp
    src/cli/cli_specs_commands.cpp
    src/cli/memory_commands.cpp
    src/cli/plugins_commands.cpp
    src/cli/schedule_commands.cpp
    src/cli/storage_commands.cpp
    src/cli/subagents_commands.cpp
    src/cli/trust_commands.cpp
    src/auth/auth_manager.cpp
    src/auth/auth_models.cpp
    src/auth/auth_profile_store.cpp
    src/auth/credential_broker.cpp
    src/auth/oauth_pkce.cpp
    src/auth/provider_adapters.cpp
    src/auth/secure_token_store.cpp
    src/auth/session_store.cpp
    src/trust/allowlist_store.cpp
    src/trust/identity_manager.cpp
    src/trust/pairing_invite_store.cpp
    src/trust/pairing_manager.cpp
    src/trust/trust_models.cpp
    src/trust/trust_policy.cpp
    src/utils/command_utils.cpp
    src/utils/atomic_file.cpp
    src/utils/json_utils.cpp
    src/utils/path_utils.cpp
    src/utils/secret_redaction.cpp
    src/utils/spec_parsing.cpp
    src/core/registry/skill_registry.cpp
    src/core/registry/agent_registry.cpp
    src/core/execution/execution_cache.cpp
    src/core/execution/task_lifecycle.cpp
    src/core/policy/approval_store.cpp
    src/core/policy/permission_model.cpp
    src/core/policy/policy_engine.cpp
    src/core/policy/role_catalog.cpp
    src/core/schema/schema_validator.cpp
    src/core/audit/audit_logger.cpp
    src/core/orchestration/agent_result_normalizer.cpp
    src/core/orchestration/subagent_manager.cpp
    src/core/orchestration/workspace_session.cpp
    src/memory/lesson_store.cpp
    src/memory/lesson_hints.cpp
    src/memory/memory_manager.cpp
    src/memory/workflow_store.cpp
    src/memory/workflow_validation.cpp
    src/storage/storage_export.cpp
    src/storage/storage_policy.cpp
    src/storage/storage_transaction.cpp
    src/storage/storage_version_store.cpp
    src/scheduler/scheduler.cpp
    src/core/router/router.cpp
    src/core/router/agent_router.cpp
    src/core/router/skill_router.cpp
    src/core/router/workflow_router.cpp
    src/core/loop/agent_loop.cpp
    src/hosts/agents/codex_cli_agent.cpp
    src/hosts/agents/gemini_agent.cpp
    src/hosts/agents/anthropic_agent.cpp
    src/hosts/agents/qwen_agent.cpp
    src/hosts/cli/cli_host.cpp
    src/hosts/cli/cli_skill_invoker.cpp
    src/hosts/cli/cli_spec_loader.cpp
    src/hosts/plugin/plugin_execution.cpp
    src/hosts/plugin/plugin_health.cpp
    src/hosts/plugin/plugin_host.cpp
    src/hosts/plugin/plugin_json_rpc.cpp
    src/hosts/plugin/plugin_json_utils.cpp
    src/hosts/plugin/plugin_manifest_loader.cpp
    src/hosts/plugin/plugin_sandbox.cpp
    src/hosts/plugin/plugin_schema_validator.cpp
    src/hosts/plugin/plugin_skill_invoker.cpp
    src/hosts/plugin/plugin_spec_utils.cpp
    src/skills/builtin/file_read_skill.cpp
    src/skills/builtin/file_write_skill.cpp
    src/skills/builtin/file_patch_skill.cpp
    src/skills/builtin/http_fetch_skill.cpp
    src/skills/builtin/workflow_run_skill.cpp
    src/hosts/agents/local_planning_agent.cpp
)

add_executable(agentos src/main.cpp)
target_include_directories(agentos_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(agentos_core PUBLIC Advapi32 Ws2_32)
endif()
target_link_libraries(agentos PRIVATE agentos_core)

enable_testing()

add_executable(agentos_cli_integration_tests tests/cli_integration_tests.cpp)
target_compile_definitions(agentos_cli_integration_tests PRIVATE AGENTOS_CLI_TEST_EXE="$<TARGET_FILE:agentos>")
add_dependencies(agentos_cli_integration_tests agentos)
add_test(NAME agentos_cli_integration_tests COMMAND agentos_cli_integration_tests)

add_executable(agentos_storage_tests tests/storage_tests.cpp)
target_link_libraries(agentos_storage_tests PRIVATE agentos_core)
add_test(NAME agentos_storage_tests COMMAND agentos_storage_tests)

add_executable(agentos_auth_tests tests/auth_tests.cpp)
target_link_libraries(agentos_auth_tests PRIVATE agentos_core)
add_test(NAME agentos_auth_tests COMMAND agentos_auth_tests)

add_executable(agentos_agent_provider_tests tests/agent_provider_tests.cpp)
target_link_libraries(agentos_agent_provider_tests PRIVATE agentos_core)
add_test(NAME agentos_agent_provider_tests COMMAND agentos_agent_provider_tests)

add_executable(agentos_policy_trust_tests tests/policy_trust_tests.cpp)
target_link_libraries(agentos_policy_trust_tests PRIVATE agentos_core)
add_test(NAME agentos_policy_trust_tests COMMAND agentos_policy_trust_tests)

add_executable(agentos_scheduler_tests tests/scheduler_tests.cpp)
target_link_libraries(agentos_scheduler_tests PRIVATE agentos_core)
add_test(NAME agentos_scheduler_tests COMMAND agentos_scheduler_tests)

add_executable(agentos_workflow_router_tests tests/workflow_router_tests.cpp)
target_link_libraries(agentos_workflow_router_tests PRIVATE agentos_core)
add_test(NAME agentos_workflow_router_tests COMMAND agentos_workflow_router_tests)

add_executable(agentos_subagent_session_tests tests/subagent_session_tests.cpp)
target_link_libraries(agentos_subagent_session_tests PRIVATE agentos_core)
add_test(NAME agentos_subagent_session_tests COMMAND agentos_subagent_session_tests)

add_executable(agentos_cli_plugin_tests tests/cli_plugin_tests.cpp)
target_link_libraries(agentos_cli_plugin_tests PRIVATE agentos_core)
add_test(NAME agentos_cli_plugin_tests COMMAND agentos_cli_plugin_tests)

add_executable(agentos_spec_parsing_tests tests/spec_parsing_tests.cpp)
target_link_libraries(agentos_spec_parsing_tests PRIVATE agentos_core)
add_test(NAME agentos_spec_parsing_tests COMMAND agentos_spec_parsing_tests)

add_executable(agentos_file_skill_policy_tests tests/file_skill_policy_tests.cpp)
target_link_libraries(agentos_file_skill_policy_tests PRIVATE agentos_core)
add_test(NAME agentos_file_skill_policy_tests COMMAND agentos_file_skill_policy_tests)

if(MSVC)
    target_compile_options(agentos_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_cli_integration_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_storage_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_auth_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_agent_provider_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_policy_trust_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_scheduler_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_workflow_router_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_subagent_session_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_cli_plugin_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_spec_parsing_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_file_skill_policy_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(agentos_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_cli_integration_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_storage_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_auth_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_agent_provider_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_policy_trust_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_workflow_router_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_subagent_session_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_cli_plugin_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_spec_parsing_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_file_skill_policy_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
