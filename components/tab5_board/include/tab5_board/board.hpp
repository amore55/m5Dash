// Board abstraction for the M5Stack Tab5.
//
// This is a thin, deliberately boring wrapper over `espressif/m5stack_tab5` (the official
// Espressif BSP). Its whole job is to:
//
//   * bring the board up in the right order, once,
//   * put the display into 1280x720 landscape,
//   * hand out an RAII lock for the LVGL mutex,
//   * own the RTC and the backlight schedule,
//   * expose the one non-obvious step nobody remembers: powering the ESP32-C6 Wi-Fi rail.
//
// It does NOT re-implement anything the BSP already does. In particular the BSP's
// bsp_display_start() already calls bsp_i2c_init() and
// bsp_feature_enable(BSP_FEATURE_LCD/TOUCH, true) internally, so we must not.

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

#include "tab5_board/backlight.hpp"
#include "tab5_board/rtc_rx8130.hpp"

namespace tab5 {

/// RAII guard for the LVGL mutex owned by esp_lvgl_port.
///
/// EVERY access to an lv_obj_t from outside an LVGL timer/event callback must be inside one
/// of these. Inside an lv_timer callback the lock is already held, so taking it again would
/// be wrong — esp_lvgl_port's mutex is recursive, but relying on that hides bugs, so the
/// convention in this codebase is: UI code called from tick()/event callbacks does NOT lock;
/// everything else does.
class LvglLock {
  public:
    /// timeout_ms == 0 means wait forever.
    explicit LvglLock(uint32_t timeout_ms = 0);
    ~LvglLock();

    LvglLock(const LvglLock&) = delete;
    LvglLock& operator=(const LvglLock&) = delete;
    LvglLock(LvglLock&&) = delete;
    LvglLock& operator=(LvglLock&&) = delete;

    /// False if the timeout expired. Callers that pass a timeout MUST check this.
    bool held() const { return held_; }
    explicit operator bool() const { return held_; }

  private:
    bool held_ = false;
};

class Board {
  public:
    /// Single instance — there is exactly one board.
    static Board& instance();

    /// Bring up display, touch and LVGL, rotate to landscape, start the backlight, and
    /// attach the RTC. Safe to call once; subsequent calls return ESP_OK immediately.
    ///
    /// Failure of the RTC is NOT fatal: the dashboard runs without it (NTP only).
    /// Failure of the display IS fatal and is reported as such.
    esp_err_t init();

    bool initialised() const { return display_ != nullptr; }

    lv_display_t* display() const { return display_; }

    /// Logical screen size after rotation (1280 x 720).
    int32_t width() const;
    int32_t height() const;

    Backlight& backlight() { return backlight_; }
    Rx8130& rtc() { return rtc_; }

    /// Power the ESP32-C6 wireless coprocessor.
    ///
    /// MUST be called before esp_wifi_init(). BSP_WIFI_EN lives on the board's *second*
    /// PI4IOE5V6408 IO expander; without this the SDIO link to the C6 never comes up and
    /// esp_wifi_init() fails in a way that looks like a software problem.
    esp_err_t enableWifiRail();

    /// Blank / unblank the panel. Used by the deep-dim path, not by the schedule.
    esp_err_t sleepDisplay();
    esp_err_t wakeDisplay();

  private:
    Board() = default;

    lv_display_t* display_ = nullptr;
    Backlight backlight_;
    Rx8130 rtc_;
    bool wifi_rail_on_ = false;
};

}  // namespace tab5
