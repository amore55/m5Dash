#include "tab5_board/board.hpp"

#include <cinttypes>

#include "bsp/esp-bsp.h"
#include "esp_log.h"

namespace tab5 {
namespace {

constexpr const char* kTag = "board";

/// Height in lines of each LVGL draw buffer. Mirrors CONFIG_BSP_LCD_DRAW_BUF_HEIGHT but is
/// set explicitly here so the memory cost is visible at the call site:
///   BSP_LCD_H_RES(720) x 60 lines x 2 bytes = 86 KB, doubled = 173 KB of internal DMA RAM.
constexpr uint32_t kDrawBufferLines = 60;

}  // namespace

LvglLock::LvglLock(uint32_t timeout_ms) { held_ = bsp_display_lock(timeout_ms); }

LvglLock::~LvglLock() {
    if (held_) {
        bsp_display_unlock();
    }
}

Board& Board::instance() {
    static Board board;
    return board;
}

esp_err_t Board::init() {
    if (display_ != nullptr) {
        return ESP_OK;
    }

    // Explicit config rather than bsp_display_start() so the buffer strategy is documented
    // in our source rather than hidden in a Kconfig default.
    //
    // sw_rotate is required: the panel is physically 720x1280 portrait and we want a
    // 1280x720 landscape UI. esp_lvgl_port only permits LV_DISPLAY_ROTATION_90 when
    // software rotation is enabled.
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * kDrawBufferLines,
        .double_buffer = true,
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    // bsp_display_start_with_config() internally: lvgl_port_init() -> LCD init (which calls
    // bsp_feature_enable(BSP_FEATURE_LCD, true)) -> touch init (which calls bsp_i2c_init()
    // and bsp_feature_enable(BSP_FEATURE_TOUCH, true)). Do not duplicate any of that.
    display_ = bsp_display_start_with_config(&cfg);
    if (display_ == nullptr) {
        ESP_LOGE(kTag, "bsp_display_start_with_config failed — display and touch are dead");
        return ESP_FAIL;
    }

    {
        LvglLock lock;
        bsp_display_rotate(display_, LV_DISPLAY_ROTATION_90);
    }
    ESP_LOGI(kTag, "display up: panel %dx%d, UI %" PRId32 "x%" PRId32 " (rotated 90 deg)",
             BSP_LCD_H_RES, BSP_LCD_V_RES, width(), height());

    esp_err_t err = backlight_.init();
    if (err != ESP_OK) {
        // Non-fatal: a dark screen is better than no boot, and this is recoverable.
        ESP_LOGW(kTag, "backlight init failed: %s", esp_err_to_name(err));
    }

    // The RTC shares the BSP's I2C bus, which bsp_display_start_with_config() has already
    // brought up. Absence of the RTC is tolerated.
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        ESP_LOGW(kTag, "no I2C bus handle; RTC unavailable");
    } else if (rtc_.attach(bus) != ESP_OK) {
        ESP_LOGW(kTag, "RTC unavailable; time will depend on NTP only");
    }

    return ESP_OK;
}

int32_t Board::width() const {
    return display_ != nullptr ? lv_display_get_horizontal_resolution(display_) : 0;
}

int32_t Board::height() const {
    return display_ != nullptr ? lv_display_get_vertical_resolution(display_) : 0;
}

esp_err_t Board::enableWifiRail() {
    if (wifi_rail_on_) {
        return ESP_OK;
    }
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to power the ESP32-C6 rail: %s", esp_err_to_name(err));
        return err;
    }
    wifi_rail_on_ = true;
    ESP_LOGI(kTag, "ESP32-C6 wireless rail enabled");
    return ESP_OK;
}

esp_err_t Board::sleepDisplay() { return bsp_display_enter_sleep(); }

esp_err_t Board::wakeDisplay() { return bsp_display_exit_sleep(); }

}  // namespace tab5
