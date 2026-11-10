cmake_minimum_required(VERSION 3.16)

project(tiny-agent
    VERSION 0.1
    DESCRIPTION "A lightweight AI Agent written in C"
    LANGUAGES C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


include(CTest)

function(tiny_agent_enable_warnings target)
    if (MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4 /WX /std:c17 /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -std=c17 -Wall -Wextra -Wpedantic -Werror -pedantic-errors
            -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
            -Wvla -Wundef -Wshadow -Wformat=2)
    endif()
endfunction()

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")


add_subdirectory(src)

add_subdirectory(app)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()