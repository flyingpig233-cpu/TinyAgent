# cJSON is a PUBLIC dependency of TinyAgent::Core because agent/tool.h includes
# <cjson/cJSON.h>; linking the core target propagates its include dirs and
# library. This keeps tests independent of pkg-config, which is unavailable on
# MSVC and vcpkg builds.

function(tiny_agent_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE TinyAgent::Core)
    tiny_agent_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    if (ARGC GREATER 2)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${ARGV2})
    endif()
endfunction()

tiny_agent_add_test(test_agent test_agent.c)
tiny_agent_add_test(test_mock_llm test_mock_llm.c)
tiny_agent_add_test(test_agent_run test_agent_run.c)
tiny_agent_add_test(test_agent_events test_agent_events.c)
tiny_agent_add_test(test_tool_approval test_tool_approval.c)
tiny_agent_add_test(test_error_handling test_error_handling.c)
tiny_agent_add_test(test_session test_session.c)
tiny_agent_add_test(test_openai_provider test_openai_provider.c)
tiny_agent_add_test(test_openai_json test_openai_json.c)
tiny_agent_add_test(test_openai_responses test_openai_responses.c)
tiny_agent_add_test(test_tool_registry test_tool_registry.c)
tiny_agent_add_test(test_cancellation test_cancellation.c 15)

add_executable(test_platform test_platform.c)
target_include_directories(test_platform PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_platform PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_platform)
add_test(NAME test_platform COMMAND test_platform)

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_executable(test_http_client test_http_client.c)
    target_link_libraries(test_http_client PRIVATE TinyAgent::Core)
    tiny_agent_enable_warnings(test_http_client)
    add_test(NAME test_http_client
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/http_server_test.py $<TARGET_FILE:test_http_client>)

    add_executable(test_openai_complete test_openai_complete.c)
    target_link_libraries(test_openai_complete PRIVATE TinyAgent::Core)
    tiny_agent_enable_warnings(test_openai_complete)
    add_test(NAME test_openai_complete
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/openai_server_test.py $<TARGET_FILE:test_openai_complete>)

    add_test(NAME test_cli_json
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_json_test.py $<TARGET_FILE:tiny-agent>)
    set_tests_properties(test_cli_json PROPERTIES TIMEOUT 30)
    add_test(NAME test_cli_stream
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_stream_test.py $<TARGET_FILE:tiny-agent>)
    set_tests_properties(test_cli_stream PROPERTIES TIMEOUT 30)
endif()

# These tests drive POSIX file descriptors, process groups, signals or a PTY.
if (UNIX)
    tiny_agent_add_test(test_read_file test_read_file.c)
    tiny_agent_add_test(test_list_dir test_list_dir.c)
    tiny_agent_add_test(test_coding_tools test_coding_tools.c 15)

    if (Python3_Interpreter_FOUND)
        add_test(NAME test_cli_cancel
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_cancel_test.py $<TARGET_FILE:tiny-agent>)
        set_tests_properties(test_cli_cancel PROPERTIES TIMEOUT 40)
        add_test(NAME test_cli_tui
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_tui_test.py $<TARGET_FILE:tiny-agent>)
        set_tests_properties(test_cli_tui PROPERTIES TIMEOUT 60)
        add_test(NAME test_cli_tui_approval
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_tui_approval_test.py $<TARGET_FILE:tiny-agent>)
        set_tests_properties(test_cli_tui_approval PROPERTIES TIMEOUT 60)
    endif()
endif()
