#include "tab5_board/backlight.hpp"

#include <algorithm>

#include "bsp/esp-bsp.h"
#include "esp_log.h"

namespace tab5 {
namespace {
constexpr const char* kTag = "backlight";
}

bool inDimWindow(int minute, int start_min, int end_min) {
    if (start_min < 0 || end_min < 0 || start_min == end_min) {
        return false;  // schedule disabled
    }
    if (start_min < end_min) {
        return minute >= start_min && minute < end_min;
    }
    // Window wraps midnight, e.g. 22:30 -> 07:00.
    return minute >= start_min || minute < end_min;
}

esp_err_t Backlight::init() {
    esp_err_t err = bsp_display_brightness_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "bsp_display_brightness_init failed: %s", esp_err_to_name(err));
        return err;
    }
    return apply(day_percent_);
}

void Backlight::configure(int day_percent, int night_percent, int dim_start_min,
                          int dim_end_min) {
    day_percent_ = std::clamp(day_percent, kMinPercent, 100);
    night_percent_ = std::clamp(night_percent, kMinPercent, 100);
    dim_start_min_ = dim_start_min;
    dim_end_min_ = dim_end_min;
}

int Backlight::scheduledPercent(int local_minutes_since_midnight) const {
    return inDimWindow(local_minutes_since_midnight, dim_start_min_, dim_end_min_)
               ? night_percent_
               : day_percent_;
}

esp_err_t Backlight::applyForLocalTime(int local_minutes_since_midnight) {
    const bool night = inDimWindow(local_minutes_since_midnight, dim_start_min_, dim_end_min_);
    const int target = night ? night_percent_ : day_percent_;
    night_active_ = night;
    if (target == applied_percent_) {
        return ESP_OK;
    }
    return apply(target);
}

esp_err_t Backlight::setTemporary(int percent) {
    return apply(std::clamp(percent, kMinPercent, 100));
}

esp_err_t Backlight::apply(int percent) {
    esp_err_t err;
    if (percent <= 0) {
        // bsp_display_brightness_set(0) leaves the LEDC output driving 0%, which on some
        // panels still shows a faint glow. Explicitly gate the backlight instead.
        err = bsp_display_backlight_off();
    } else {
        err = bsp_display_brightness_set(percent);
    }
    if (err == ESP_OK) {
        applied_percent_ = percent;
        ESP_LOGD(kTag, "brightness -> %d%%", percent);
    } else {
        ESP_LOGW(kTag, "failed to set brightness %d%%: %s", percent, esp_err_to_name(err));
    }
    return err;
}

}  // namespace tab5
