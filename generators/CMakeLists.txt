file(GLOB generator_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")
add_library(forma_generators STATIC ${generator_sources})
add_library(forma::generators ALIAS forma_generators)
target_include_directories(forma_generators
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
target_link_libraries(forma_generators PUBLIC forma::code_generation forma::layout)
forma_apply_warnings(forma_generators)
