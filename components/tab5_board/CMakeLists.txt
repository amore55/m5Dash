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
        # We drive lvgl_port directly rather than using bsp_display_start_with_config(),
        # because that helper hard-codes a DSI lane bit rate this panel will not lock onto.
        # See kDsiLaneBitRateMbps in src/board.cpp.
        espressif__esp_lvgl_port
        # A PRIVATE dependency of the BSP, so it is not re-exported to us — but bsp/touch.h
        # needs its types and we attach the touch device to LVGL ourselves.
        espressif__esp_lcd_touch
        # Vendored Sitronix ST7121 driver. Espressif's BSP has no ST7121 support and
        # mis-detects those panels as ST7123, which leaves the screen permanently dark.
        esp_lcd_st7121
        esp_driver_i2c
        esp_lcd          # esp_lcd_panel_ops.h / esp_lcd_mipi_dsi.h
        esp_hw_support   # esp_ldo_regulator.h — powering the MIPI-DSI PHY ourselves
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
