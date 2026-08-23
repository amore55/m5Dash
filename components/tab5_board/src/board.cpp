#include "tab5_board/board.hpp"

#include <cinttypes>

// MUST come before bsp/esp-bsp.h. That header pulls in bsp/touch.h, which declares
// bsp_touch_new() using esp_lcd_touch_handle_t without declaring the type itself.
#include "esp_lcd_touch.h"

#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_touch.h"

namespace tab5 {
namespace {

constexpr const char* kTag = "board";

// ---------------------------------------------------------------------------------------
// PANEL VARIANTS
//
// Retail Tab5 units ship with one of three panels. Espressif's BSP knows two of them and
// mis-identifies the third, which is why this file does the panel bring-up itself rather than
// calling bsp_display_start_with_config(). See components/esp_lcd_st7121/CMakeLists.txt for
// the full story.
//
// Detection mirrors M5Stack's own firmware: probe the touch controller, and for the 0x55
// family read its firmware-version register — 1 means ST7121, 3 means ST7123.
// ---------------------------------------------------------------------------------------
enum class PanelType {
    Ili9881c,  ///< "board version 1", GT911 touch. Handled by the BSP.
    St7123,    ///< "board version 2", fw version 3. Handled by the BSP.
    St7121,    ///< ALSO reports as "version 2", fw version 1. NOT handled by the BSP.
};

constexpr uint8_t kSt712xTouchAddress = 0x55;
constexpr uint16_t kTouchFwVersionReg = 0x0000;
constexpr uint8_t kFwVersionSt7121 = 1;
constexpr uint8_t kFwVersionSt7123 = 3;

// ---------------------------------------------------------------------------------------
// THE COLD-BOOT DARK-SCREEN BUG
//
// Symptom: from the wall socket the backlight lights and the panel stays blank forever, while
// the firmware behind it runs perfectly — Wi-Fi up, HTTP answering, plugins fetching. Flash or
// reset over USB and the screen works every time. Six weeks of testing never saw it, because
// every test boot was a soft reset.
//
// Cause: detection identifies the panel by reading the TOUCH controller, and on this board the
// touch controller cannot be read until the LCD RAIL is live. BSP_LCD_RST is GPIO_NUM_NC and
// bsp_touch_new() notes that touch reset is "usually shared with LCD reset", so with the LCD
// rail down the controller is held in reset and NACKs everything. Board::init() used to assert
// only BSP_FEATURE_TOUCH before probing and leave the LCD rail to panel creation, which happens
// afterwards — so the probe could never succeed from cold.
//
// It succeeded on a warm reset for a second, compounding reason: the IO expander carrying both
// rails is a separate chip on I2C with its own supply, and a CPU reset does not clear its
// outputs. After a soft reset BOTH rails are still high from the previous run and the controller
// has been awake for as long as the device has been plugged in.
//
// Why that produced a blank screen rather than an error: a failed probe was not treated as a
// failure. It was read as evidence about which board this is, and fell through to ILI9881C — so
// an ST7121 was driven by the wrong driver at the wrong timings. The LCD rail, the DSI PHY and
// the backlight all come up flawlessly and the panel never receives a signal it can lock to.
//
// Measured on the failing unit: probe never answered in 774 ms with the LCD rail down; the BSP's
// own touch driver then read firmware version 1 — kFwVersionSt7121, the panel we had just ruled
// out — about 500 ms after panel creation brought that rail up.
//
// Two rules follow, and both are needed:
//   * assert the LCD rail BEFORE probing (see Board::init), and
//   * treat probe failure as "not ready yet" until the budget is spent, and only then as
//     "not present".
// ---------------------------------------------------------------------------------------

/// Settle time before the first probe, covering the controller's own power-on reset.
constexpr uint32_t kTouchSettleMs = 50;

/// Total time the controller is allowed to take before it is judged absent.
///
/// Generous on purpose, and asymmetric in cost: an ST712x answers as soon as it is ready and
/// returns immediately, so this budget is only ever spent in full on a genuine ILI9881C board,
/// where it delays boot once. Giving up early costs a device that never displays anything.
constexpr uint32_t kTouchDetectBudgetMs = 1500;

/// Gap between retries, for both the probe and the version read.
constexpr uint32_t kTouchRetryMs = 25;

/// A controller that answers a probe may still not serve its firmware register on the first ask.
constexpr int kVersionReadAttempts = 5;

/// Wait for the ST712x touch controller to answer at 0x55, and report how long that took.
///
/// Returns false only once kTouchDetectBudgetMs has elapsed with no answer, which is the one
/// case that genuinely means "there is no ST712x here".
bool waitForTouchController(i2c_master_bus_handle_t bus, uint32_t& waited_ms) {
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline_us = start_us + static_cast<int64_t>(kTouchDetectBudgetMs) * 1000;

    vTaskDelay(pdMS_TO_TICKS(kTouchSettleMs));
    for (;;) {
        const bool answered = i2c_master_probe(bus, kSt712xTouchAddress, 100) == ESP_OK;
        const int64_t now_us = esp_timer_get_time();
        waited_ms = static_cast<uint32_t>((now_us - start_us) / 1000);
        if (answered) {
            return true;
        }
        if (now_us >= deadline_us) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(kTouchRetryMs));
    }
}

/// MIPI-DSI lane bit rate for the ST7121, in Mbps.
/// 965, taken from M5Stack's working firmware. The BSP's 1000 is for the other panels.
constexpr uint32_t kSt7121LaneBitRateMbps = 965;
constexpr uint32_t kSt7121DpiClockMhz = 70;

/// Height in lines of each LVGL draw buffer. THREE of these are allocated, not two: software
/// rotation needs one of its own (esp_lvgl_port_disp.c:474), and they live in internal DMA RAM.
///   720 px x 40 lines x 2 bytes = 57.6 KB each, x3 = 173 KB of a ~243 KB largest block.
/// Above ~53 lines the third allocation fails at boot.
constexpr uint32_t kDrawBufferLines = 40;

/// Rotate the 720x1280 portrait panel to a 1280x720 landscape UI.
/// Set false to bisect display problems — it removes the rotation path entirely.
constexpr bool kRotateToLandscape = true;

const char* toString(PanelType type) {
    switch (type) {
        case PanelType::Ili9881c:
            return "ILI9881C";
        case PanelType::St7123:
            return "ST7123";
        case PanelType::St7121:
            return "ST7121";
    }
    return "?";
}

/// Identify the panel from the touch controller, exactly as M5Stack's firmware does.
///
/// Requires the I2C bus and the touch power rail to be up already. Falls back to ST7123 for an
/// unrecognised 0x55 device, because that is what the BSP would have assumed anyway.
PanelType detectPanel() {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        ESP_LOGW(kTag, "no I2C bus for panel detection; assuming ILI9881C");
        return PanelType::Ili9881c;
    }

    uint32_t waited_ms = 0;
    if (!waitForTouchController(bus, waited_ms)) {
        // No ST712x touch controller after a full power-on budget: the older GT911 board.
        // Logged with the wait, because on an ST712x unit this line is the dark-screen symptom.
        ESP_LOGW(kTag, "no ST712x touch controller after %" PRIu32 " ms; assuming ILI9881C",
                 waited_ms);
        return PanelType::Ili9881c;
    }
    ESP_LOGI(kTag, "touch controller answered after %" PRIu32 " ms", waited_ms);

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kSt712xTouchAddress;
    dev_cfg.scl_speed_hz = 100000;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGW(kTag, "could not open touch controller; assuming ST7123");
        return PanelType::St7123;
    }

    // 16-bit big-endian register address, then one byte of data.
    const uint8_t reg[2] = {static_cast<uint8_t>(kTouchFwVersionReg >> 8),
                            static_cast<uint8_t>(kTouchFwVersionReg & 0xFF)};
    uint8_t version = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < kVersionReadAttempts && err != ESP_OK; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(kTouchRetryMs));
        }
        err = i2c_master_transmit_receive(dev, reg, sizeof(reg), &version, 1, 200);
    }
    i2c_master_bus_rm_device(dev);

    if (err != ESP_OK) {
        ESP_LOGW(kTag, "touch version read failed (%s); assuming ST7123", esp_err_to_name(err));
        return PanelType::St7123;
    }

    if (version == kFwVersionSt7121) {
        return PanelType::St7121;
    }
    if (version != kFwVersionSt7123) {
        ESP_LOGW(kTag, "unrecognised touch firmware version %u; assuming ST7123", version);
    }
    return PanelType::St7123;
}

/// Power the MIPI-DSI PHY. The BSP does this internally but does not expose it, and the
/// ST7121 path does not go through the BSP's panel creation.
esp_err_t enableDsiPhyPower(esp_ldo_channel_handle_t* out_chan) {
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN;
    ldo_cfg.voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV;
    return esp_ldo_acquire_channel(&ldo_cfg, out_chan);
}

/// Bring up an ST7121 panel by hand, using M5Stack's proven parameters.
///
/// Mirrors bsp_display_new_with_handles() but with the ST7121 driver, its DPI timings and the
/// 965 Mbps lane rate. The init command sequence is the driver's own default
/// (vendor_specific_init_default in esp_lcd_st7121.c), which is why init_cmds is left null.
esp_err_t createSt7121Panel(bsp_lcd_handles_t* handles) {
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_LCD, true), kTag, "LCD rail failed");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), kTag, "brightness init failed");

    esp_ldo_channel_handle_t ldo = nullptr;
    ESP_RETURN_ON_ERROR(enableDsiPhyPower(&ldo), kTag, "DSI PHY power failed");

    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = kSt7121LaneBitRateMbps;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &handles->mipi_dsi_bus), kTag,
                        "DSI bus failed");

    esp_lcd_dbi_io_config_t dbi_config = {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(handles->mipi_dsi_bus, &dbi_config, &handles->io),
                        kTag, "panel IO failed");

    // ST7121 video timings, from M5Stack's firmware. These differ from the ST7123's only in
    // the vertical porches, but that difference is the whole ball game: get them wrong and the
    // panel never locks onto the signal.
    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = kSt7121DpiClockMhz;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS;
    dpi_config.video_timing.h_size = BSP_LCD_H_RES;
    dpi_config.video_timing.v_size = BSP_LCD_V_RES;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_back_porch = 40;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_pulse_width = 20;
    dpi_config.video_timing.vsync_back_porch = 24;
    dpi_config.video_timing.vsync_front_porch = 200;
    dpi_config.flags.use_dma2d = true;

    st7121_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = nullptr;  // use the driver's built-in ST7121 sequence
    vendor_config.init_cmds_size = 0;
    vendor_config.mipi_config.dsi_bus = handles->mipi_dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config = {};
    // No hardware reset line, matching M5Stack. The init sequence performs a software reset.
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = BSP_LCD_COLOR_SPACE;
    panel_config.bits_per_pixel = BSP_LCD_BITS_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7121(handles->io, &panel_config, &handles->panel),
                        kTag, "ST7121 panel creation failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(handles->panel), kTag, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(handles->panel), kTag, "panel init failed");
    esp_lcd_panel_invert_color(handles->panel, false);
    esp_lcd_panel_mirror(handles->panel, false, false);
    esp_lcd_panel_disp_on_off(handles->panel, true);

    ESP_LOGI(kTag, "ST7121 panel up: %dx%d, DSI %" PRIu32 " Mbps x%d lanes, DPI %" PRIu32 " MHz",
             BSP_LCD_H_RES, BSP_LCD_V_RES, kSt7121LaneBitRateMbps, BSP_LCD_MIPI_DSI_LANE_NUM,
             kSt7121DpiClockMhz);
    return ESP_OK;
}

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

    // I2C and the touch rail must be up before the panel can be identified, because the
    // identification is done by reading the touch controller.
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), kTag, "I2C init failed");

    // BOTH rails, and the LCD one FIRST — see the note above waitForTouchController(). The touch
    // controller has no reset line of its own (BSP_LCD_RST is GPIO_NUM_NC, and bsp_touch_new notes
    // its reset is "usually shared with LCD reset"), so from cold it stays mute until the LCD rail
    // is live. Enabling only BSP_FEATURE_TOUCH here and letting panel creation bring up the LCD
    // rail afterwards is what made cold-boot detection impossible.
    //
    // Both are idempotent — createSt7121Panel() and bsp_display_new_with_handles() each assert the
    // LCD rail again — so doing it early costs nothing.
    const esp_err_t lcd_rail = bsp_feature_enable(BSP_FEATURE_LCD, true);
    if (lcd_rail != ESP_OK) {
        ESP_LOGW(kTag, "LCD rail enable failed: %s", esp_err_to_name(lcd_rail));
    }
    const esp_err_t touch_rail = bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    if (touch_rail != ESP_OK) {
        ESP_LOGW(kTag, "touch rail enable failed: %s", esp_err_to_name(touch_rail));
    }

    const PanelType panel_type = detectPanel();
    ESP_LOGI(kTag, "detected panel: %s", toString(panel_type));

    bsp_lcd_handles_t handles = {};
    if (panel_type == PanelType::St7121) {
        // Espressif's BSP has no ST7121 support and would silently initialise it as an
        // ST7123, producing a permanently dark screen. Do it ourselves.
        ESP_RETURN_ON_ERROR(createSt7121Panel(&handles), kTag, "ST7121 bring-up failed");
    } else {
        bsp_display_config_t hw_cfg = {};
        hw_cfg.dsi_bus.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
        hw_cfg.dsi_bus.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
        ESP_RETURN_ON_ERROR(bsp_display_new_with_handles(&hw_cfg, &handles), kTag,
                            "panel init failed");
        esp_lcd_panel_disp_on_off(handles.panel, true);
    }

    if (backlight_.init() != ESP_OK) {
        ESP_LOGW(kTag, "backlight init failed; continuing with a dark panel");
    }

    // ---- LVGL -------------------------------------------------------------------------
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), kTag, "lvgl_port_init failed");

    // Field-by-field rather than a designated initialiser: C++ requires those to follow
    // declaration order, and this struct's layout belongs to the BSP, not to us.
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = handles.io;
    disp_cfg.panel_handle = handles.panel;
    disp_cfg.buffer_size = BSP_LCD_H_RES * kDrawBufferLines;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = BSP_LCD_H_RES;
    disp_cfg.vres = BSP_LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.buff_spiram = false;
    disp_cfg.flags.swap_bytes = (BSP_LCD_BIGENDIAN ? 1 : 0);
    disp_cfg.flags.sw_rotate = kRotateToLandscape;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = false;

    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(kTag, "lvgl_port_add_disp_dsi failed — no display");
        return ESP_FAIL;
    }

    // ---- Touch. Not fatal: an unswipeable dashboard is still readable. -----------------
    esp_lcd_touch_handle_t touch = nullptr;
    esp_err_t err = bsp_touch_new(nullptr, &touch);
    if (err == ESP_OK && touch != nullptr) {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = display_;
        touch_cfg.handle = touch;
        if (lvgl_port_add_touch(&touch_cfg) == nullptr) {
            ESP_LOGW(kTag, "touch could not be attached to LVGL");
        }
    } else {
        ESP_LOGW(kTag, "touch unavailable (%s); navigation will not work", esp_err_to_name(err));
    }

    // ---- Orientation ------------------------------------------------------------------
    if (kRotateToLandscape) {
        LvglLock lock;
        bsp_display_rotate(display_, LV_DISPLAY_ROTATION_90);
    }
    ESP_LOGI(kTag, "display up: panel %dx%d, UI %" PRId32 "x%" PRId32 " (%s)", BSP_LCD_H_RES,
             BSP_LCD_V_RES, width(), height(),
             kRotateToLandscape ? "rotated 90 deg" : "native portrait");

    // ---- RTC. Absence is tolerated: the dashboard falls back to network time. -----------
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
