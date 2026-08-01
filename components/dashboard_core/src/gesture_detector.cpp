#include "dashboard/gesture_detector.hpp"

#include <cinttypes>

#include "esp_log.h"

#include "app_config.hpp"

namespace dashboard {
namespace {

constexpr const char* kTag = "gesture";

int32_t absOf(int32_t value) { return value < 0 ? -value : value; }

/// Find the first pointer-type input device. Lets dashboard_core stay independent of the BSP.
lv_indev_t* findPointerIndev() {
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return nullptr;
}

}  // namespace

const char* toString(Gesture gesture) {
    switch (gesture) {
        case Gesture::SwipeLeft:
            return "swipe-left";
        case Gesture::SwipeRight:
            return "swipe-right";
        case Gesture::SwipeDown:
            return "swipe-down";
        case Gesture::SwipeUp:
            return "swipe-up";
        case Gesture::LongPress:
            return "long-press";
    }
    return "?";
}

GestureDetector::~GestureDetector() { stop(); }

esp_err_t GestureDetector::start(Callback callback, lv_indev_t* indev) {
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer_ != nullptr) {
        return ESP_OK;  // idempotent
    }

    indev_ = (indev != nullptr) ? indev : findPointerIndev();
    if (indev_ == nullptr) {
        // Not fatal: the dashboard is still readable, it just cannot be navigated by touch.
        // Worth a loud warning because it means the BSP's touch init did not produce a device.
        ESP_LOGE(kTag, "no pointer input device found — touch navigation is unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    callback_ = std::move(callback);
    timer_ = lv_timer_create(&GestureDetector::timerCb, kPollPeriodMs, this);
    if (timer_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag,
             "gesture detection active (swipe %" PRId32 " px, long press %" PRIu32
             " ms, cooldown %" PRIu32 " ms)",
             dash::cfg::kSwipeMinDistancePx, dash::cfg::kLongPressMs,
             dash::cfg::kGestureCooldownMs);
    return ESP_OK;
}

void GestureDetector::stop() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    callback_ = nullptr;
    pressing_ = false;
}

void GestureDetector::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        // Abandon any press in progress, so re-enabling mid-drag cannot produce a gesture
        // measured from a stale origin.
        pressing_ = false;
        consumed_ = false;
    }
}

void GestureDetector::timerCb(lv_timer_t* timer) {
    auto* self = static_cast<GestureDetector*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->poll();
    }
}

bool GestureDetector::inCooldown() const {
    return cooldown_active_ && lv_tick_elaps(cooldown_started_tick_) < dash::cfg::kGestureCooldownMs;
}

void GestureDetector::poll() {
    if (indev_ == nullptr) {
        return;
    }

    const bool down = (lv_indev_get_state(indev_) == LV_INDEV_STATE_PRESSED);

    if (down && !pressing_) {
        // ---- press began ----
        pressing_ = true;
        consumed_ = false;
        lv_indev_get_point(indev_, &press_point_);
        last_point_ = press_point_;
        press_started_tick_ = lv_tick_get();
        max_deviation_ = 0;

        // Disarm long-press if the finger landed on something interactive. See the header.
        lv_obj_t* target = lv_indev_get_active_obj();
        press_on_clickable_ =
            (target != nullptr) && lv_obj_has_flag(target, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    if (down && pressing_) {
        // ---- press continuing ----
        lv_indev_get_point(indev_, &last_point_);
        const int32_t dx = absOf(last_point_.x - press_point_.x);
        const int32_t dy = absOf(last_point_.y - press_point_.y);
        const int32_t deviation = (dx > dy) ? dx : dy;
        if (deviation > max_deviation_) {
            max_deviation_ = deviation;
        }

        if (!consumed_ && enabled_ && long_press_enabled_ && !press_on_clickable_ &&
            !inCooldown() && max_deviation_ <= dash::cfg::kLongPressSlopPx &&
            lv_tick_elaps(press_started_tick_) >= dash::cfg::kLongPressMs) {
            // Fire on the way down rather than waiting for release: a long press should feel
            // like it has been acknowledged while the finger is still there.
            emit(Gesture::LongPress);
        }
        return;
    }

    if (!down && pressing_) {
        // ---- press released ----
        pressing_ = false;
        if (!consumed_ && enabled_) {
            classifyRelease();
        }
        consumed_ = false;
    }
}

void GestureDetector::classifyRelease() {
    if (inCooldown()) {
        return;
    }

    const uint32_t duration = lv_tick_elaps(press_started_tick_);
    if (duration > dash::cfg::kSwipeMaxDurationMs) {
        // Too slow to be a flick. Treated as a drag/scroll so that a plugin's own scrollable
        // list still behaves normally.
        return;
    }

    const int32_t dx = last_point_.x - press_point_.x;
    const int32_t dy = last_point_.y - press_point_.y;
    const int32_t adx = absOf(dx);
    const int32_t ady = absOf(dy);

    // Dominance test, done with integer cross-multiplication to avoid floating point:
    //   adx / ady  >  numerator / denominator
    // Requiring horizontal travel to clearly exceed vertical (and vice versa) is what stops a
    // sloppy diagonal drag from both changing page and triggering a refresh.
    const bool horizontal_dominant =
        adx * dash::cfg::kSwipeDominanceDenominator > ady * dash::cfg::kSwipeDominanceNumerator;
    const bool vertical_dominant =
        ady * dash::cfg::kSwipeDominanceDenominator > adx * dash::cfg::kSwipeDominanceNumerator;

    if (horizontal_dominant && adx >= dash::cfg::kSwipeMinDistancePx) {
        emit(dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight);
        return;
    }

    if (vertical_dominant && ady >= dash::cfg::kPullRefreshMinDistancePx) {
        emit(dy > 0 ? Gesture::SwipeDown : Gesture::SwipeUp);
    }
}

void GestureDetector::emit(Gesture gesture) {
    consumed_ = true;
    cooldown_active_ = true;
    cooldown_started_tick_ = lv_tick_get();

    // Cancel any widget interaction this drag started, so a swipe beginning on a button does
    // not also deliver a click when the finger lifts.
    if (indev_ != nullptr) {
        lv_indev_reset(indev_, nullptr);
    }

    ESP_LOGD(kTag, "%s", toString(gesture));

    if (callback_) {
        callback_(gesture);
    }
}

}  // namespace dashboard
