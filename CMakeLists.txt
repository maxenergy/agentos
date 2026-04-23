cmake_minimum_required(VERSION 3.20)

project(agentos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(agentos_core
    src/auth/auth_manager.cpp
    src/auth/auth_models.cpp
    src/auth/auth_profile_store.cpp
    src/auth/credential_broker.cpp
    src/auth/provider_adapters.cpp
    src/auth/secure_token_store.cpp
    src/auth/session_store.cpp
    src/trust/allowlist_store.cpp
    src/trust/identity_manager.cpp
    src/trust/pairing_manager.cpp
    src/trust/trust_models.cpp
    src/trust/trust_policy.cpp
    src/utils/command_utils.cpp
    src/utils/json_utils.cpp
    src/utils/path_utils.cpp
    src/core/registry/skill_registry.cpp
    src/core/registry/agent_registry.cpp
    src/core/execution/execution_cache.cpp
    src/core/policy/permission_model.cpp
    src/core/policy/policy_engine.cpp
    src/core/audit/audit_logger.cpp
    src/core/orchestration/subagent_manager.cpp
    src/core/orchestration/workspace_session.cpp
    src/memory/lesson_store.cpp
    src/memory/memory_manager.cpp
    src/memory/workflow_store.cpp
    src/scheduler/scheduler.cpp
    src/core/router/router.cpp
    src/core/router/router_components.cpp
    src/core/loop/agent_loop.cpp
    src/hosts/agents/codex_cli_agent.cpp
    src/hosts/cli/cli_host.cpp
    src/hosts/cli/cli_skill_invoker.cpp
    src/skills/builtin/file_read_skill.cpp
    src/skills/builtin/file_write_skill.cpp
    src/skills/builtin/file_patch_skill.cpp
    src/skills/builtin/http_fetch_skill.cpp
    src/skills/builtin/workflow_run_skill.cpp
    src/hosts/agents/mock_planning_agent.cpp
)

add_executable(agentos src/main.cpp)
target_include_directories(agentos_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(agentos PRIVATE agentos_core)

enable_testing()

add_executable(agentos_smoke_tests tests/core_smoke_tests.cpp)
target_link_libraries(agentos_smoke_tests PRIVATE agentos_core)
add_test(NAME agentos_smoke_tests COMMAND agentos_smoke_tests)

if(MSVC)
    target_compile_options(agentos_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_smoke_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(agentos_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_smoke_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
