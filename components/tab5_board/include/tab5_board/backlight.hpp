// Backlight brightness control.
//
// Scope split, deliberately: this class owns **how bright**, the application owns **when**.
//
// An earlier draft had the dim schedule (start/end times, midnight wrap) in here. That was
// moved out to dashboard::timeutil::inTimeWindow because the same predicate is needed for the
// TfL commute windows, and tab5_board cannot depend on dashboard_core without creating a
// circular component dependency. Keeping the time policy in one place beats duplicating six
// lines of modular arithmetic across a layering boundary.
//
// So the caller decides night vs day and tells us; we translate that into a panel level and
// avoid touching the hardware when nothing changed.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace tab5 {

class Backlight {
  public:
    /// 0 gates the backlight off entirely rather than driving the LEDC at 0%, which on some
    /// panels still leaves a faint glow.
    static constexpr int kMinPercent = 0;
    static constexpr int kMaxPercent = 100;

    /// Initialises the LEDC channel and applies the current day level.
    esp_err_t init();

    /// Both values are clamped to [0, 100]. Takes effect on the next applyNightMode() call,
    /// or immediately if the currently active mode's level changed.
    void configure(int day_percent, int night_percent);

    /// Select the day or night level. Cheap to call repeatedly — the panel is only written
    /// when the resulting level differs from what is already applied, so this can safely be
    /// driven from the 250 ms UI tick.
    esp_err_t applyNightMode(bool night);

    /// Force a level, overriding the current mode until the next applyNightMode() call that
    /// resolves to a different level. Used by the Settings brightness slider so dragging it
    /// gives immediate feedback without permanently defeating the schedule.
    esp_err_t setTemporary(int percent);

    /// Bring the panel to the DAY level for `duration_ms`, then let the schedule resume.
    ///
    /// This is what makes a night level of 0 survivable. Without it, a dimmed-to-black dashboard is
    /// indistinguishable from a dead one — which is exactly how this was first reported: "it
    /// doesn't boot, it stays black", on a device that was running perfectly and answering HTTP.
    ///
    /// Calling it again while a wake is running extends the deadline rather than stacking, so
    /// keeping a finger on the screen keeps it lit.
    void wake(uint32_t duration_ms);

    /// True while a wake() is still holding the panel up.
    bool waking() const;

    int currentPercent() const { return applied_percent_; }
    int dayPercent() const { return day_percent_; }
    int nightPercent() const { return night_percent_; }
    bool nightActive() const { return night_active_; }

  private:
    esp_err_t apply(int percent);

    int day_percent_ = 70;
    int night_percent_ = 12;
    int applied_percent_ = -1;  // -1 = nothing applied yet, so the first apply always writes
    bool night_active_ = false;

    /// esp_timer microseconds at which an active wake expires. 0 = not waking.
    /// Signed 64-bit, matching esp_timer_get_time(), so it cannot wrap in any realistic uptime.
    int64_t wake_until_us_ = 0;
};

}  // namespace tab5
