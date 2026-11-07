add_executable(test_agent test_agent.c)

target_link_libraries(test_agent PRIVATE TinyAgent::Core)

tiny_agent_enable_warnings(test_agent)

add_test(NAME test_agent COMMAND test_agent)
add_executable(test_mock_llm test_mock_llm.c)
target_link_libraries(test_mock_llm PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_mock_llm)
add_test(NAME test_mock_llm COMMAND test_mock_llm)

add_executable(test_agent_run test_agent_run.c)
target_link_libraries(test_agent_run PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_agent_run)
add_test(NAME test_agent_run COMMAND test_agent_run)

add_executable(test_agent_events test_agent_events.c)
target_link_libraries(test_agent_events PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_agent_events)
add_test(NAME test_agent_events COMMAND test_agent_events)

add_executable(test_tool_approval test_tool_approval.c)
target_link_libraries(test_tool_approval PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_tool_approval)
add_test(NAME test_tool_approval COMMAND test_tool_approval)

add_executable(test_error_handling test_error_handling.c)
target_link_libraries(test_error_handling PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_error_handling)
add_test(NAME test_error_handling COMMAND test_error_handling)

add_executable(test_session test_session.c)
target_link_libraries(test_session PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_session)
add_test(NAME test_session COMMAND test_session)

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_executable(test_http_client test_http_client.c)
    target_link_libraries(test_http_client PRIVATE TinyAgent::Core)
    tiny_agent_enable_warnings(test_http_client)
    add_test(NAME test_http_client
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/http_server_test.py $<TARGET_FILE:test_http_client>)
endif()

add_executable(test_openai_provider test_openai_provider.c)
target_link_libraries(test_openai_provider PRIVATE TinyAgent::Core)
tiny_agent_enable_warnings(test_openai_provider)
add_test(NAME test_openai_provider COMMAND test_openai_provider)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
add_executable(test_openai_json test_openai_json.c)
target_link_libraries(test_openai_json PRIVATE TinyAgent::Core PkgConfig::CJSON)
tiny_agent_enable_warnings(test_openai_json)
add_test(NAME test_openai_json COMMAND test_openai_json)

add_executable(test_openai_responses test_openai_responses.c)
target_link_libraries(test_openai_responses PRIVATE TinyAgent::Core PkgConfig::CJSON)
tiny_agent_enable_warnings(test_openai_responses)
add_test(NAME test_openai_responses COMMAND test_openai_responses)

if (Python3_Interpreter_FOUND)
    add_executable(test_openai_complete test_openai_complete.c)
    target_link_libraries(test_openai_complete PRIVATE TinyAgent::Core)
    tiny_agent_enable_warnings(test_openai_complete)
    add_test(NAME test_openai_complete
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/openai_server_test.py $<TARGET_FILE:test_openai_complete>)
endif()

add_executable(test_tool_registry test_tool_registry.c)
target_link_libraries(test_tool_registry PRIVATE TinyAgent::Core PkgConfig::CJSON)
tiny_agent_enable_warnings(test_tool_registry)
add_test(NAME test_tool_registry COMMAND test_tool_registry)

add_executable(test_list_dir test_list_dir.c)
target_link_libraries(test_list_dir PRIVATE TinyAgent::Core PkgConfig::CJSON)
tiny_agent_enable_warnings(test_list_dir)
add_test(NAME test_list_dir COMMAND test_list_dir)

add_executable(test_read_file test_read_file.c)
target_link_libraries(test_read_file PRIVATE TinyAgent::Core PkgConfig::CJSON)
tiny_agent_enable_warnings(test_read_file)
add_test(NAME test_read_file COMMAND test_read_file)

add_executable(test_coding_tools test_coding_tools.c)
target_link_libraries(test_coding_tools PRIVATE TinyAgent::Core PkgConfig::CJSON)
tiny_agent_enable_warnings(test_coding_tools)
add_test(NAME test_coding_tools COMMAND test_coding_tools)
set_tests_properties(test_coding_tools PROPERTIES TIMEOUT 15)

if (Python3_Interpreter_FOUND)
    add_test(NAME test_cli_json
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_json_test.py $<TARGET_FILE:tiny-agent>)
    set_tests_properties(test_cli_json PROPERTIES TIMEOUT 30)
    add_test(NAME test_cli_stream
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_stream_test.py $<TARGET_FILE:tiny-agent>)
    set_tests_properties(test_cli_stream PROPERTIES TIMEOUT 30)
endif()
