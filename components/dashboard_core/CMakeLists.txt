idf_component_register(
    SRCS
        "src/semver.cpp"
        "src/time_utils.cpp"
        "src/json_util.cpp"
        "src/network_indicator.cpp"
        "src/theme.cpp"
        "src/page_scaffold.cpp"
        "src/worker.cpp"
        "src/plugin_base.cpp"
        "src/gesture_detector.cpp"
        "src/page_manager.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        lvgl__lvgl
        json            # ESP-IDF's bundled cJSON
        esp_timer
    PRIV_REQUIRES
        tab5_board      # LvglLock only
        esp_hw_support
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
