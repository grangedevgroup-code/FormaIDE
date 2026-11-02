forma_add_module(core
    PUBLIC_DEPS nlohmann_json::nlohmann_json
    PRIVATE_DEPS forma_sqlite3)

if(WIN32)
    target_link_libraries(forma_core PRIVATE advapi32 shell32 ole32)
elseif(APPLE)
    target_link_libraries(forma_core PRIVATE "-framework CoreServices" "-framework CoreFoundation" "-framework Security")
elseif(UNIX AND NOT EMSCRIPTEN)
    target_link_libraries(forma_core PRIVATE ${CMAKE_DL_LIBS} util)
endif()

find_package(Threads REQUIRED)
target_link_libraries(forma_core PUBLIC Threads::Threads)
target_precompile_headers(forma_core PUBLIC
    "$<$<COMPILE_LANGUAGE:CXX>:<nlohmann/json.hpp$<ANGLE-R>>"
    "$<$<COMPILE_LANGUAGE:CXX>:<string$<ANGLE-R>>"
    "$<$<COMPILE_LANGUAGE:CXX>:<vector$<ANGLE-R>>")
