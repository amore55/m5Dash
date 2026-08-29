idf_component_register(
    SRCS
        "src/calendar_model.cpp"
        "src/graph_calendar_provider.cpp"
        "src/calendar_plugin.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        dashboard_network
        dashboard_storage
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
