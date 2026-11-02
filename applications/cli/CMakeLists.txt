add_executable(forma main.cpp)
target_link_libraries(forma PRIVATE forma::project forma::generators forma::resources)
forma_apply_warnings(forma)
install(TARGETS forma RUNTIME DESTINATION bin)
