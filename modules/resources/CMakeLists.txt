include(FormaEmbed)
set(embedded_source "${CMAKE_CURRENT_BINARY_DIR}/embedded_data.cpp")
forma_embed_directory("${embedded_source}" "${PROJECT_SOURCE_DIR}/resources/data")
forma_add_module(resources
    PUBLIC_DEPS forma::core
    EXTRA_SOURCES "${embedded_source}")
