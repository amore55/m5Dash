idf_component_register(
    SRCS
        "src/settings.cpp"
        "src/settings_migrate.cpp"
        "src/settings_store.cpp"
        "src/secret_store.cpp"
        "src/fs.cpp"
        "src/task_store.cpp"
        "src/cache_store.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        dashboard_core          # FixedString, JSON helpers, time utilities
        nvs_flash
        joltwallet__littlefs
        esp_partition
        vfs
    PRIV_REQUIRES
        mbedtls                 # SHA-256 for the salted lock-screen PIN hash
        esp_hw_support          # esp_random.h, for the PIN salt
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
