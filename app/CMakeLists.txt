set(TINY_AGENT_TUI_SOURCES)
if (NOT WIN32)
    set(CURSES_NEED_NCURSES TRUE)
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses REQUIRED)
    list(APPEND TINY_AGENT_TUI_SOURCES tui.c)
endif()

add_executable(tiny-agent
    main.c
    ui.c
    ${TINY_AGENT_TUI_SOURCES}
)

if (WIN32)
    # The full-screen backend requires ncursesw; Windows uses line mode.
    target_compile_definitions(tiny-agent PRIVATE TINY_AGENT_NO_TUI=1)
else()
    target_include_directories(tiny-agent PRIVATE ${CURSES_INCLUDE_DIRS})
    target_compile_options(tiny-agent PRIVATE ${CURSES_CFLAGS})
    target_link_libraries(tiny-agent PRIVATE ${CURSES_LIBRARIES})
endif()

target_include_directories(tiny-agent PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tiny-agent PRIVATE TinyAgent::Core)

tiny_agent_enable_warnings(tiny-agent)
