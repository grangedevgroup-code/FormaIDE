cmake_minimum_required(VERSION 3.25)

project(FormaIDE VERSION 0.1.0 LANGUAGES C CXX
        DESCRIPTION "Native visual software development environment"
        HOMEPAGE_URL "https://github.com/grangedevgroup-code/FormaIDE")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FORMA_BUILD_DESKTOP "Build the Qt desktop application" ON)
option(FORMA_BUILD_CLI "Build the forma command line tool" ON)
option(FORMA_BUILD_TESTS "Build the test suites" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(FormaDependencies)
include(FormaModule)

add_subdirectory(core)
add_subdirectory(modules)
add_subdirectory(generators)

if(FORMA_BUILD_CLI AND NOT EMSCRIPTEN)
    add_subdirectory(applications/cli)
endif()

if(FORMA_BUILD_DESKTOP AND NOT EMSCRIPTEN)
    add_subdirectory(applications/desktop)
endif()

if(FORMA_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()

include(FormaPackaging)
