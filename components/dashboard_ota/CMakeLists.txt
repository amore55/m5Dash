idf_component_register(
    SRCS
        "src/manifest.cpp"
        "src/ota_service.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        dashboard_network
    PRIV_REQUIRES
        app_update       # esp_ota_ops.h — begin/write/end, partition selection, rollback
        mbedtls          # streaming SHA-256 of the image while it downloads
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
