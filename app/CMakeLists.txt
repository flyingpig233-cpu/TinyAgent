add_executable(tiny-agent
    main.c
    ui.c
)

target_link_libraries(tiny-agent PRIVATE TinyAgent::Core)


tiny_agent_enable_warnings(tiny-agent)
