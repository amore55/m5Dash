# Sitronix ST7121 MIPI-DSI panel driver.
#
# VENDORED, not pulled from the component registry, because it is not published there.
# Source: m5stack/M5Tab5-UserDemo, platforms/tab5/components/esp_lcd_st7121
# Authored by Espressif (SPDX-FileCopyrightText: 2024-2025 Espressif Systems), Apache-2.0.
# Recorded in LICENSES.md.
#
# WHY THIS EXISTS
# ---------------
# Retail M5Stack Tab5 units ship with one of THREE panels, and Espressif's official BSP
# (espressif/m5stack_tab5) only knows two of them:
#
#   ILI9881C + GT911   -> board "version 1", BSP supported
#   ST7123             -> board "version 2", BSP supported
#   ST7121             -> ALSO detected as "version 2" by the BSP, and NOT supported
#
# The BSP distinguishes revisions by probing the touch controller's I2C address. M5Stack's own
# firmware goes further and reads the touch firmware-version register: 1 means ST7121, 3 means
# ST7123. The BSP does not, so an ST7121 panel is initialised with the ST7123 command sequence
# and ST7123 video timings.
#
# The failure mode is silent and extremely misleading: esp_lcd_panel_init(),
# esp_lcd_panel_disp_on_off(), esp_lcd_panel_draw_bitmap() and even
# esp_lcd_dpi_panel_set_pattern() all return ESP_OK, the backlight lights, and the touch panel
# enumerates correctly — but the display never produces an image, because DSI command writes
# are fire-and-forget and nothing ever reports that the panel did not accept them.
#
# Remove this component if and when the upstream BSP gains ST7121 support.

idf_component_register(
    SRCS "esp_lcd_st7121.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd
)
