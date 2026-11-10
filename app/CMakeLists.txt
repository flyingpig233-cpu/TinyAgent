set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

add_executable(tiny-agent
    main.c
    tui.c
    ui.c
)

target_include_directories(tiny-agent PRIVATE ${CURSES_INCLUDE_DIRS})
target_compile_options(tiny-agent PRIVATE ${CURSES_CFLAGS})
target_link_libraries(tiny-agent PRIVATE TinyAgent::Core ${CURSES_LIBRARIES})


tiny_agent_enable_warnings(tiny-agent)
