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
