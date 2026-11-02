add_executable(forma_iconbake main.cpp)
target_link_libraries(forma_iconbake PRIVATE Qt6::Gui Qt6::Svg forma::assets)
forma_apply_warnings(forma_iconbake)
