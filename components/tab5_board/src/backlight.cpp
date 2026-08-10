#include "tab5_board/backlight.hpp"

#include <algorithm>

#include "bsp/esp-bsp.h"
#include "esp_log.h"

namespace tab5 {
namespace {
constexpr const char* kTag = "backlight";
}

esp_err_t Backlight::init() {
    // Deliberately does NOT call bsp_display_brightness_init().
    //
    // The BSP already configures the LEDC timer and channel during display bring-up
    // (bsp_display.c:175, inside bsp_display_new_with_handles). Calling it again re-runs
    // ledc_channel_config() on the same pin and the driver complains:
    //
    //   W ledc: GPIO 22 is not usable, maybe conflict with others
    //
    // Harmless in practice, but it is a false alarm in the boot log, and a boot log people
    // learn to ignore is worse than no boot log. Same rule as bsp_i2c_init() and
    // bsp_feature_enable(BSP_FEATURE_LCD/TOUCH): if the BSP does it, we must not.
    return apply(day_percent_);
}

void Backlight::configure(int day_percent, int night_percent) {
    day_percent_ = std::clamp(day_percent, kMinPercent, kMaxPercent);
    night_percent_ = std::clamp(night_percent, kMinPercent, kMaxPercent);
}

esp_err_t Backlight::applyNightMode(bool night) {
    night_active_ = night;
    const int target = night ? night_percent_ : day_percent_;
    if (target == applied_percent_) {
        return ESP_OK;
    }
    return apply(target);
}

esp_err_t Backlight::setTemporary(int percent) {
    return apply(std::clamp(percent, kMinPercent, kMaxPercent));
}

esp_err_t Backlight::apply(int percent) {
    const esp_err_t err = (percent <= 0) ? bsp_display_backlight_off()
                                         : bsp_display_brightness_set(percent);
    if (err == ESP_OK) {
        applied_percent_ = percent;
        ESP_LOGD(kTag, "brightness -> %d%%", percent);
    } else {
        ESP_LOGW(kTag, "failed to set brightness %d%%: %s", percent, esp_err_to_name(err));
    }
    return err;
}

}  // namespace tab5
