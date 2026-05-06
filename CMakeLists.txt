cmake_minimum_required(VERSION 3.20)

project(agentos VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Populate(httplib)

add_library(agentos_core
    src/autodev/autodev_job_id.cpp
    src/autodev/autodev_models.cpp
    src/autodev/autodev_skill_pack_loader.cpp
    src/autodev/autodev_state_store.cpp
    src/cli/agents_commands.cpp
    src/cli/autodev_commands.cpp
    src/cli/auth_commands.cpp
    src/cli/auth_interactive.cpp
    src/cli/cli_specs_commands.cpp
    src/cli/diagnostics_commands.cpp
    src/cli/intent_classifier.cpp
    src/cli/interactive_intent_registry.cpp
    src/cli/interactive_route_policy.cpp
    src/cli/interactive_commands.cpp
    src/cli/main_agent_commands.cpp
    src/cli/serve_commands.cpp
    src/cli/memory_commands.cpp
    src/cli/plugins_commands.cpp
    src/cli/schedule_commands.cpp
    src/cli/skill_reload.cpp
    src/cli/storage_commands.cpp
    src/cli/subagents_commands.cpp
    src/cli/trust_commands.cpp
    src/auth/auth_manager.cpp
    src/auth/auth_login_flow.cpp
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
    src/utils/cancellation.cpp
    src/utils/curl_secret.cpp
    src/utils/path_utils.cpp
    src/utils/secret_redaction.cpp
    src/utils/secure_random.cpp
    src/utils/signal_cancellation.cpp
    src/utils/spec_parsing.cpp
    src/core/registry/skill_registry.cpp
    src/core/registry/agent_registry.cpp
    src/core/execution/agent_event_runtime_store.cpp
    src/core/execution/execution_cache.cpp
    src/core/execution/task_wait_policy.cpp
    src/core/execution/task_lifecycle.cpp
    src/core/policy/approval_store.cpp
    src/core/policy/permission_model.cpp
    src/core/policy/policy_engine.cpp
    src/core/policy/role_catalog.cpp
    src/core/schema/schema_validator.cpp
    src/core/audit/audit_logger.cpp
    src/core/orchestration/agent_dispatch.cpp
    src/core/orchestration/agent_result_normalizer.cpp
    src/core/orchestration/subagent_manager.cpp
    src/core/orchestration/workspace_session.cpp
    src/memory/lesson_store.cpp
    src/memory/lesson_hints.cpp
    src/memory/memory_manager.cpp
    src/memory/workflow_store.cpp
    src/memory/workflow_validation.cpp
    src/storage/main_agent_store.cpp
    src/storage/storage_backend.cpp
    src/storage/storage_export.cpp
    src/storage/storage_policy.cpp
    src/storage/storage_transaction.cpp
    src/storage/storage_version_store.cpp
    src/scheduler/cron.cpp
    src/scheduler/scheduler.cpp
    src/scheduler/timezone.cpp
    src/core/router/router.cpp
    src/core/router/agent_router.cpp
    src/core/router/skill_router.cpp
    src/core/router/workflow_router.cpp
    src/core/loop/agent_loop.cpp
    src/hosts/agents/codex_cli_agent.cpp
    src/hosts/agents/gemini_agent.cpp
    src/hosts/agents/anthropic_agent.cpp
    src/hosts/agents/main_agent.cpp
    src/hosts/agents/qwen_agent.cpp
    src/hosts/agents/openai_agent.cpp
    src/hosts/cli/cli_host.cpp
    src/hosts/cli/cli_skill_invoker.cpp
    src/hosts/cli/cli_spec_loader.cpp
    src/hosts/plugin/plugin_execution.cpp
    src/hosts/plugin/plugin_health.cpp
    src/hosts/plugin/plugin_host.cpp
    src/hosts/plugin/plugin_json_rpc.cpp
    src/hosts/plugin/plugin_manifest_loader.cpp
    src/hosts/plugin/plugin_sandbox.cpp
    src/hosts/plugin/plugin_schema_validator.cpp
    src/hosts/plugin/plugin_skill_invoker.cpp
    src/hosts/plugin/plugin_spec_utils.cpp
    src/skills/builtin/development_skill.cpp
    src/skills/builtin/file_read_skill.cpp
    src/skills/builtin/file_write_skill.cpp
    src/skills/builtin/file_patch_skill.cpp
    src/skills/builtin/host_info_skill.cpp
    src/skills/builtin/http_fetch_skill.cpp
    src/skills/builtin/learn_skill.cpp
    src/skills/builtin/news_search_skill.cpp
    src/skills/builtin/research_skill.cpp
    src/skills/builtin/workflow_run_skill.cpp
    src/hosts/agents/local_planning_agent.cpp
)

add_executable(agentos src/main.cpp)
target_include_directories(agentos_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
if(WIN32)
    target_link_libraries(agentos_core PUBLIC Advapi32 Ws2_32 Bcrypt Iphlpapi)
endif()
if(APPLE)
    # SecureTokenStore uses the macOS Security framework for the Keychain backend.
    find_library(AGENTOS_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(AGENTOS_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(agentos_core PUBLIC
        ${AGENTOS_SECURITY_FRAMEWORK}
        ${AGENTOS_COREFOUNDATION_FRAMEWORK})
endif()
if(UNIX AND NOT APPLE)
    # libsecret is optional: if present, SecureTokenStore enables the Linux
    # Secret Service backend; otherwise we fall back to env-ref-only.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AGENTOS_LIBSECRET QUIET libsecret-1)
    endif()
    if(AGENTOS_LIBSECRET_FOUND)
        target_compile_definitions(agentos_core PUBLIC AGENTOS_HAVE_LIBSECRET=1)
        target_include_directories(agentos_core PUBLIC ${AGENTOS_LIBSECRET_INCLUDE_DIRS})
        target_link_libraries(agentos_core PUBLIC ${AGENTOS_LIBSECRET_LIBRARIES})
        target_compile_options(agentos_core PUBLIC ${AGENTOS_LIBSECRET_CFLAGS_OTHER})
        message(STATUS "AgentOS: libsecret found; enabling Linux Secret Service backend")
    else()
        message(STATUS "AgentOS: libsecret not found; SecureTokenStore will use env-ref-only fallback on Linux")
    endif()
endif()
target_link_libraries(agentos_core PUBLIC nlohmann_json::nlohmann_json)
target_include_directories(agentos_core PUBLIC ${CMAKE_BINARY_DIR}/_deps/httplib-src)
target_link_libraries(agentos PRIVATE agentos_core)
if(MSVC)
    target_compile_options(agentos_core PUBLIC /utf-8)
    target_compile_options(agentos PRIVATE /utf-8)
endif()

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

add_executable(agentos_agent_dispatch_tests tests/agent_dispatch_tests.cpp)
target_link_libraries(agentos_agent_dispatch_tests PRIVATE agentos_core)
add_test(NAME agentos_agent_dispatch_tests COMMAND agentos_agent_dispatch_tests)

add_executable(agentos_task_wait_policy_tests tests/task_wait_policy_tests.cpp)
target_link_libraries(agentos_task_wait_policy_tests PRIVATE agentos_core)
add_test(NAME agentos_task_wait_policy_tests COMMAND agentos_task_wait_policy_tests)

add_executable(agentos_intent_classifier_tests tests/intent_classifier_tests.cpp)
target_link_libraries(agentos_intent_classifier_tests PRIVATE agentos_core)
add_test(NAME agentos_intent_classifier_tests COMMAND agentos_intent_classifier_tests)

add_executable(agentos_routing_eval_tests tests/routing_eval_tests.cpp)
target_compile_definitions(agentos_routing_eval_tests PRIVATE AGENTOS_ROUTING_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(agentos_routing_eval_tests PRIVATE agentos_core)
add_test(NAME agentos_routing_eval_tests COMMAND agentos_routing_eval_tests)

add_executable(agentos_news_search_skill_tests tests/news_search_skill_tests.cpp)
target_link_libraries(agentos_news_search_skill_tests PRIVATE agentos_core)
add_test(NAME agentos_news_search_skill_tests COMMAND agentos_news_search_skill_tests)

add_executable(agentos_autodev_tests tests/autodev_tests.cpp)
target_link_libraries(agentos_autodev_tests PRIVATE agentos_core)
add_test(NAME agentos_autodev_tests COMMAND agentos_autodev_tests)

add_executable(agentos_research_development_skill_tests tests/research_development_skill_tests.cpp)
target_link_libraries(agentos_research_development_skill_tests PRIVATE agentos_core)
add_test(NAME agentos_research_development_skill_tests COMMAND agentos_research_development_skill_tests)

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

add_executable(agentos_cancellation_tests tests/cancellation_tests.cpp)
target_link_libraries(agentos_cancellation_tests PRIVATE agentos_core)
add_test(NAME agentos_cancellation_tests COMMAND agentos_cancellation_tests)

add_executable(agentos_cli_plugin_tests tests/cli_plugin_tests.cpp)
target_link_libraries(agentos_cli_plugin_tests PRIVATE agentos_core)
add_test(NAME agentos_cli_plugin_tests COMMAND agentos_cli_plugin_tests)

add_executable(agentos_spec_parsing_tests tests/spec_parsing_tests.cpp)
target_link_libraries(agentos_spec_parsing_tests PRIVATE agentos_core)
add_test(NAME agentos_spec_parsing_tests COMMAND agentos_spec_parsing_tests)

add_executable(agentos_file_skill_policy_tests tests/file_skill_policy_tests.cpp)
target_link_libraries(agentos_file_skill_policy_tests PRIVATE agentos_core)
add_test(NAME agentos_file_skill_policy_tests COMMAND agentos_file_skill_policy_tests)

add_executable(agentos_host_info_skill_tests tests/host_info_skill_tests.cpp)
target_link_libraries(agentos_host_info_skill_tests PRIVATE agentos_core)
add_test(NAME agentos_host_info_skill_tests COMMAND agentos_host_info_skill_tests)

add_executable(agentos_learn_skill_tests tests/learn_skill_tests.cpp)
target_link_libraries(agentos_learn_skill_tests PRIVATE agentos_core)
add_test(NAME agentos_learn_skill_tests COMMAND agentos_learn_skill_tests)

add_executable(agentos_chat_fallback_tests tests/chat_fallback_tests.cpp)
target_link_libraries(agentos_chat_fallback_tests PRIVATE agentos_core)
add_test(NAME agentos_chat_fallback_tests COMMAND agentos_chat_fallback_tests)

add_executable(agentos_main_agent_prompt_tests tests/main_agent_prompt_tests.cpp)
target_link_libraries(agentos_main_agent_prompt_tests PRIVATE agentos_core)
add_test(NAME agentos_main_agent_prompt_tests COMMAND agentos_main_agent_prompt_tests)

if(MSVC)
    target_compile_options(agentos_core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_cli_integration_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_storage_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_auth_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_agent_provider_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_agent_dispatch_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_task_wait_policy_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_intent_classifier_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_routing_eval_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_news_search_skill_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_autodev_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_research_development_skill_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_policy_trust_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_scheduler_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_workflow_router_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_subagent_session_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_cancellation_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_cli_plugin_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_spec_parsing_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_file_skill_policy_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_host_info_skill_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_learn_skill_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_chat_fallback_tests PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(agentos_main_agent_prompt_tests PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(agentos_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_cli_integration_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_storage_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_auth_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_agent_provider_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_agent_dispatch_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_task_wait_policy_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_intent_classifier_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_routing_eval_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_news_search_skill_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_autodev_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_research_development_skill_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_policy_trust_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_scheduler_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_workflow_router_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_subagent_session_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_cancellation_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_cli_plugin_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_spec_parsing_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_file_skill_policy_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_host_info_skill_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_learn_skill_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_chat_fallback_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(agentos_main_agent_prompt_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
