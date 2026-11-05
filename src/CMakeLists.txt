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

find_package(SQLite3 REQUIRED)

target_link_libraries(tiny_agent_core
    PRIVATE
        CURL::libcurl
        PkgConfig::CJSON
)


tiny_agent_enable_warnings(tiny_agent_core)