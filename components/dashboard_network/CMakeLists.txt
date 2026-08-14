idf_component_register(
    SRCS
        "src/wifi_manager.cpp"
        "src/web_server.cpp"
        "src/https_client.cpp"
        "src/response_buffer.cpp"
        "src/time_sync.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    # The configuration pages are real files rather than C string literals: they are HTML, CSS and
    # JavaScript, and they are far easier to read, edit and lint when the editor knows that.
    # EMBED_TXTFILES appends a NUL, so each arrives in the firmware as an ordinary C string.
    EMBED_TXTFILES
        "web/setup.html"
        "web/settings.html"
        "web/style.css"
        "web/app.js"
    REQUIRES
        dashboard_core
        dashboard_storage       # credentials and the lock PIN come from SecretStore
        tab5_board              # the ESP32-C6 power rail must be enabled before esp_wifi_init()
        esp_wifi
        esp_netif
        esp_event
        esp_http_server         # the setup portal and the settings page
        esp_http_client         # the outbound API client
        mbedtls                 # esp_crt_bundle: the root store every request validates against
        mdns                    # so the settings page has a name, not just an address
        lwip                    # SNTP
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
