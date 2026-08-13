idf_component_register(
    SRCS
        "src/wifi_manager.cpp"
        "src/provisioning.cpp"
        "src/time_sync.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core
        dashboard_storage       # credentials come from SecretStore
        tab5_board              # the ESP32-C6 power rail must be enabled before esp_wifi_init()
        esp_wifi
        esp_netif
        esp_event
        esp_http_server        # the first-run setup portal
        lwip                   # SNTP
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
