// Backlight brightness with a day/night schedule.
//
// The schedule is expressed in *local* minutes-since-midnight and is evaluated by the
// caller passing the current local time in, so this class needs no notion of timezones and
// stays trivially testable.
//
// A "dim window" that wraps midnight (e.g. 22:30 -> 07:00) is the normal case and is
// handled explicitly; getting that wrong is the classic bug in schedules like this.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace tab5 {

class Backlight {
  public:
    /// Percentages are clamped to [kMinPercent, 100]. 0 is allowed and turns the panel off.
    static constexpr int kMinPercent = 0;

    esp_err_t init();

    /// day/night levels and the dim window, in local minutes since midnight.
    /// If start == end the schedule is disabled and the day level is always used.
    void configure(int day_percent, int night_percent, int dim_start_min, int dim_end_min);

    /// Evaluate the schedule for a local time and apply the result.
    /// Cheap to call repeatedly: the panel is only touched when the level actually changes.
    esp_err_t applyForLocalTime(int local_minutes_since_midnight);

    /// Force a level, ignoring the schedule until the next applyForLocalTime() call whose
    /// scheduled level differs from the current one. Used by the Settings brightness slider
    /// so dragging it gives immediate feedback without permanently defeating the schedule.
    esp_err_t setTemporary(int percent);

    /// Pure schedule evaluation, exposed for testing.
    /// Returns the brightness that should be active at the given local time.
    int scheduledPercent(int local_minutes_since_midnight) const;

    int currentPercent() const { return applied_percent_; }
    bool nightActive() const { return night_active_; }

  private:
    esp_err_t apply(int percent);

    int day_percent_ = 70;
    int night_percent_ = 12;
    int dim_start_min_ = -1;
    int dim_end_min_ = -1;
    int applied_percent_ = -1;
    bool night_active_ = false;
};

/// True when `minute` falls inside the [start, end) window, handling a window that wraps
/// midnight. Free function so the host tests can exercise it without a Backlight instance.
bool inDimWindow(int minute, int start_min, int end_min);

}  // namespace tab5
