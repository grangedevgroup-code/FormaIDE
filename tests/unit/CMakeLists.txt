file(GLOB unit_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(forma_unit_tests ${unit_sources})
target_link_libraries(forma_unit_tests PRIVATE forma::core GTest::gtest_main)
forma_apply_warnings(forma_unit_tests)

include(GoogleTest)
gtest_discover_tests(forma_unit_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
