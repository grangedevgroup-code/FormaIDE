include(FetchContent)

set(FETCHCONTENT_QUIET ON)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(sqlite_amalgamation
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA256=0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite_amalgamation)

add_library(forma_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(forma_sqlite3 SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(forma_sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1)
if(MSVC)
    target_compile_options(forma_sqlite3 PRIVATE /W0)
else()
    target_compile_options(forma_sqlite3 PRIVATE -w)
endif()

if(FORMA_BUILD_TESTS AND NOT EMSCRIPTEN)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
        URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

if(FORMA_BUILD_DESKTOP AND NOT EMSCRIPTEN)
    FetchContent_Declare(scintilla
        URL https://www.scintilla.org/scintilla569.tgz
        URL_HASH SHA256=1622546ce1d3804b589292f1adae98ad1d87fa8c43bb401edc098adae12dcacb
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(lexilla
        URL https://www.scintilla.org/lexilla555.tgz
        URL_HASH SHA256=fa4093ad5c48acf49a6f3eca1b75143a5c824ef1a6f5b0ee59e8c3d6a970f229
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(scintilla lexilla)
endif()
