idf_component_register(
    SRCS
        "src/board.cpp"
        "src/rtc_rx8130.cpp"
        "src/backlight.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_SOURCE_DIR}/config"
    REQUIRES
        # The official Espressif BSP for the M5Stack Tab5. Everything display, touch, LVGL,
        # backlight, I2C and IO-expander related comes from here — see
        # docs/IMPLEMENTATION_PLAN.md §2/§3. Declared in main/idf_component.yml.
        espressif__m5stack_tab5
        esp_driver_i2c
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
