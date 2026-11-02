find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg Network Core5Compat)

set(CMAKE_AUTOMOC ON)

set(scintilla_qt "${scintilla_SOURCE_DIR}/qt/ScintillaEditBase")
file(GLOB scintilla_core "${scintilla_SOURCE_DIR}/src/*.cxx")
add_library(forma_scintilla STATIC
    ${scintilla_core}
    "${scintilla_qt}/PlatQt.cpp"
    "${scintilla_qt}/ScintillaQt.cpp"
    "${scintilla_qt}/ScintillaEditBase.cpp"
    "${scintilla_qt}/PlatQt.h"
    "${scintilla_qt}/ScintillaQt.h"
    "${scintilla_qt}/ScintillaEditBase.h")
target_include_directories(forma_scintilla SYSTEM PUBLIC
    "${scintilla_SOURCE_DIR}/include"
    "${scintilla_SOURCE_DIR}/src"
    "${scintilla_qt}")
target_compile_definitions(forma_scintilla PUBLIC SCINTILLA_QT=1 MAKING_LIBRARY=1)
target_link_libraries(forma_scintilla PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Core5Compat)
if(MSVC)
    target_compile_options(forma_scintilla PRIVATE /W0 /utf-8 /EHsc)
else()
    target_compile_options(forma_scintilla PRIVATE -w)
endif()

file(GLOB lexilla_sources
    "${lexilla_SOURCE_DIR}/lexers/*.cxx"
    "${lexilla_SOURCE_DIR}/lexlib/*.cxx"
    "${lexilla_SOURCE_DIR}/src/Lexilla.cxx")
add_library(forma_lexilla STATIC ${lexilla_sources})
target_include_directories(forma_lexilla SYSTEM PUBLIC
    "${lexilla_SOURCE_DIR}/include"
    "${lexilla_SOURCE_DIR}/lexlib"
    "${scintilla_SOURCE_DIR}/include")
if(MSVC)
    target_compile_options(forma_lexilla PRIVATE /W0 /utf-8 /EHsc)
    target_compile_definitions(forma_lexilla PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(forma_lexilla PRIVATE -w)
endif()

file(GLOB_RECURSE desktop_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
add_library(forma_desktop STATIC ${desktop_sources})
target_include_directories(forma_desktop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(forma_desktop PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Network
    forma_scintilla forma_lexilla
    forma::project forma::generators forma::resources forma::canvas forma::design forma::layout
    forma::code_editor forma::terminal forma::version_control forma::lsp forma::debugger forma::ai forma::plugins forma::assets)
target_compile_definitions(forma_desktop PUBLIC QT_NO_KEYWORDS FORMA_VERSION="${PROJECT_VERSION}")
forma_apply_warnings(forma_desktop)

if(WIN32 AND EXISTS "${PROJECT_SOURCE_DIR}/packaging/windows/formaide.rc")
    set(app_icon_resource "${PROJECT_SOURCE_DIR}/packaging/windows/formaide.rc")
elseif(APPLE AND EXISTS "${PROJECT_SOURCE_DIR}/packaging/macos/FormaIDE.icns")
    set(app_icon_resource "${PROJECT_SOURCE_DIR}/packaging/macos/FormaIDE.icns")
    set_source_files_properties("${app_icon_resource}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

add_executable(FormaIDE WIN32 MACOSX_BUNDLE main.cpp ${app_icon_resource})
target_link_libraries(FormaIDE PRIVATE forma_desktop)
forma_apply_warnings(FormaIDE)
set_target_properties(FormaIDE PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "FormaIDE"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.formaide.FormaIDE"
    MACOSX_BUNDLE_ICON_FILE "FormaIDE.icns"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

install(TARGETS FormaIDE BUNDLE DESTINATION . RUNTIME DESTINATION bin)

add_subdirectory("${PROJECT_SOURCE_DIR}/tools/iconbake" "${CMAKE_BINARY_DIR}/tools/iconbake")
