// Touch gesture recognition for page navigation.
//
// WHY NOT LV_EVENT_GESTURE: LVGL's built-in gesture events are delivered to the object under
// the finger. On a page containing any button, list row or slider, that object consumes the
// event and the page never sees the swipe — so navigation silently stops working wherever the
// UI is most interactive. Bubbling helps only if you control every child, which a plugin
// architecture by definition does not.
//
// Instead this polls the input device directly on an LVGL timer and runs its own state machine,
// entirely independent of hit-testing. Widgets keep working because a tap produces no gesture;
// when a gesture IS recognised we call lv_indev_reset() to cancel whatever widget interaction
// was in progress, so a swipe that started on a button does not also click it.
//
// All thresholds live in config/app_config.hpp so navigation feel can be tuned in one place
// once it has been used on real glass.

#pragma once

#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "lvgl.h"

namespace dashboard {

enum class Gesture : uint8_t {
    SwipeLeft,   ///< Next page.
    SwipeRight,  ///< Previous page.
    SwipeDown,   ///< Manual refresh.
    SwipeUp,     ///< Reserved; currently unused.
    LongPress,   ///< Open Settings.
};

const char* toString(Gesture gesture);

class GestureDetector {
  public:
    using Callback = std::function<void(Gesture)>;

    /// Poll cadence. 30 ms is ~33 Hz: fast enough to time a 900 ms long press to within one
    /// frame, cheap enough to be invisible in the LVGL task's budget.
    static constexpr uint32_t kPollPeriodMs = 30;

    ~GestureDetector();

    /// `indev` may be null, in which case the first pointer-type input device is found
    /// automatically — which avoids dashboard_core needing to know about the BSP.
    /// The callback runs on the LVGL thread with the lock held.
    esp_err_t start(Callback callback, lv_indev_t* indev = nullptr);
    void stop();

    /// Suspend recognition entirely — used while a modal dialog is open, so a stray swipe
    /// cannot navigate away from a confirmation the user has not answered.
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    /// Long-press-to-open-Settings can be suppressed per page (the Settings page itself, or a
    /// page that wants long-press for its own purpose).
    void setLongPressEnabled(bool enabled) { long_press_enabled_ = enabled; }

  private:
    static void timerCb(lv_timer_t* timer);
    void poll();
    void emit(Gesture gesture);
    void classifyRelease();
    bool inCooldown() const;

    Callback callback_;
    lv_indev_t* indev_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    bool enabled_ = true;
    bool long_press_enabled_ = true;

    // Press state
    bool pressing_ = false;
    lv_point_t press_point_{};
    lv_point_t last_point_{};
    uint32_t press_started_tick_ = 0;
    int32_t max_deviation_ = 0;

    /// Set once a gesture has fired for the current press, so one drag cannot produce two.
    bool consumed_ = false;

    /// True when the press landed on an interactive widget. Long-press is disarmed in that
    /// case so that long-pressing a task row cannot be hijacked into opening Settings.
    /// Swipes remain armed — the dominance check keeps them distinguishable from a scroll.
    bool press_on_clickable_ = false;

    bool cooldown_active_ = false;
    uint32_t cooldown_started_tick_ = 0;
};

}  // namespace dashboard
