forma_add_module(terminal PUBLIC_DEPS forma::core)
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(forma_terminal PRIVATE util)
endif()
