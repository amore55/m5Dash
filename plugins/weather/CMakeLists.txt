idf_component_register(
    SRCS
        "src/weather_model.cpp"
        "src/open_meteo.cpp"
        "src/weather_plugin.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        dashboard_network       # HttpsClient — the only route to the internet
        dashboard_storage       # CacheStore: last good forecast survives a reboot
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
