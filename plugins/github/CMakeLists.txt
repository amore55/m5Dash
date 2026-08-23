idf_component_register(
    SRCS
        "src/github_model.cpp"
        "src/github_provider.cpp"
        "src/github_plugin.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        dashboard_network
        dashboard_storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
