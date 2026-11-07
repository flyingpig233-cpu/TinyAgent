add_library(tiny_agent_core STATIC
    core/agent.c
    context/context.c
    llm/llm.c
    llm/mock_llm.c
    infra/string.c
    infra/error.c
    config.c
    context/session.c
    infra/http_client.c
    llm/providers/openai.c
    llm/providers/openai_chat.c
    llm/providers/openai_responses.c
    llm/providers/openai_json.c
    llm/providers/openai_stream.c
    tools/tool.c
    tools/file_system/list_dir.c
    tools/file_system/read_file.c
    tools/common.c
    tools/process.c
    tools/file_system/search.c
    tools/file_system/write_file.c
    tools/file_system/edit_file.c
    tools/run_command.c
)

add_library(TinyAgent::Core ALIAS tiny_agent_core)

target_include_directories(tiny_agent_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)


find_package(CURL REQUIRED)

find_package(PkgConfig REQUIRED)

pkg_check_modules(
    CJSON
    REQUIRED
    IMPORTED_TARGET
    libcjson
)

# find_package(SQLite3 REQUIRED)

target_link_libraries(tiny_agent_core
    PRIVATE
        CURL::libcurl
        PkgConfig::CJSON
)


tiny_agent_enable_warnings(tiny_agent_core)
