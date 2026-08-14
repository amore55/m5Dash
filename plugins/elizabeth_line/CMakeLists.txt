idf_component_register(
    SRCS
        "src/elizabeth_model.cpp"
        "src/tfl_provider.cpp"
        "src/elizabeth_plugin.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        dashboard_network       # HttpsClient and the PSRAM response buffer
        dashboard_storage       # the optional TfL app key, and the status cache
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
