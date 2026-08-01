#include "dashboard/page_manager.hpp"

#include <cstring>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/theme.hpp"

namespace dashboard {
namespace {

constexpr const char* kTag = "pages";

/// Page transition length. Short enough not to feel like an animation, long enough to signal
/// that something changed. A cross-fade rather than a slide: at 1280x720 with software
/// rotation, sliding two full-screen pages costs far more than it conveys.
constexpr uint32_t kTransitionMs = 140;

/// Vertical offset of the page indicator from the bottom edge, chosen to sit inside the footer
/// strip without overlapping the footer's status text.
constexpr int32_t kIndicatorBottomOffset = -14;

}  // namespace

// ---------------------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------------------

esp_err_t PageManager::begin(lv_display_t* display) {
    if (display == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    theme::applyGlobalTheme(display);

    screen_ = lv_screen_active();
    if (screen_ == nullptr) {
        ESP_LOGE(kTag, "no active screen");
        return ESP_FAIL;
    }

    // The screen itself is a plain dark canvas. Pages are absolutely-sized full-screen children
    // rather than flex items, so a hidden page cannot perturb the layout of the visible one.
    theme::makePlain(screen_);
    lv_obj_set_style_bg_color(screen_, theme::bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, LV_PART_MAIN);

    // Indicator on the top layer so it survives page switches. Explicitly non-clickable so
    // hit-testing falls through to the page underneath.
    lv_obj_t* top = lv_layer_top();
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
    indicator_ = lv_obj_create(top);
    theme::makePlain(indicator_);
    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(indicator_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(indicator_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(indicator_, theme::kGapS, LV_PART_MAIN);
    lv_obj_align(indicator_, LV_ALIGN_BOTTOM_MID, 0, kIndicatorBottomOffset);

    esp_err_t err = gestures_.start([this](Gesture gesture) { onGesture(gesture); });
    if (err != ESP_OK) {
        // Navigation is dead but the dashboard still displays. Deliberately not fatal — a
        // read-only clock is far better than a boot loop.
        ESP_LOGE(kTag, "gesture detector unavailable (%s); pages cannot be navigated",
                 esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t PageManager::add(DashboardPlugin* plugin, bool in_rotation) {
    if (plugin == nullptr || plugin->id() == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (entry_count_ >= kMaxPlugins) {
        ESP_LOGE(kTag, "cannot register '%s': limit of %u plugins reached", plugin->id(),
                 static_cast<unsigned>(kMaxPlugins));
        return ESP_ERR_NO_MEM;
    }
    if (findIndex(plugin->id()) >= 0) {
        ESP_LOGE(kTag, "duplicate plugin id '%s'", plugin->id());
        return ESP_ERR_INVALID_STATE;
    }

    Entry& entry = entries_[entry_count_];
    entry.plugin = plugin;
    entry.in_rotation = in_rotation;
    // A plugin that failed to initialise registers as disabled, so it occupies no slot in the
    // rotation but its page still exists and explains itself.
    entry.enabled = (plugin->state() != DataState::Disabled);

    order_[order_count_++] = entry_count_;
    ++entry_count_;

    ESP_LOGI(kTag, "registered '%s' (%s%s)", plugin->id(),
             in_rotation ? "in rotation" : "overlay", entry.enabled ? "" : ", disabled");
    return ESP_OK;
}

esp_err_t PageManager::startPages(const char* default_page_id) {
    if (screen_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // Build every page up front. See the header for why pages are never destroyed.
    for (size_t i = 0; i < entry_count_; ++i) {
        Entry& entry = entries_[i];
        entry.page = lv_obj_create(screen_);
        if (entry.page == nullptr) {
            ESP_LOGE(kTag, "failed to allocate page for '%s'", entry.plugin->id());
            return ESP_ERR_NO_MEM;
        }
        lv_obj_align(entry.page, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(entry.page, LV_OBJ_FLAG_HIDDEN);
        entry.plugin->createPage(entry.page);
    }

    rebuildRotation();

    tick_timer_ = lv_timer_create(&PageManager::tickCb, dash::cfg::kUiTickPeriodMs, this);
    if (tick_timer_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Prefer the configured default page; fall back to the first page in the rotation.
    int target = (default_page_id != nullptr) ? findIndex(default_page_id) : -1;
    if (target >= 0 && (!entries_[target].enabled || !entries_[target].in_rotation)) {
        ESP_LOGW(kTag, "default page '%s' is disabled or not in rotation; ignoring",
                 default_page_id);
        target = -1;
    }
    if (target < 0 && rotation_count_ > 0) {
        target = static_cast<int>(rotation_[0]);
    }
    if (target < 0) {
        ESP_LOGE(kTag, "no enabled pages in the rotation");
        return ESP_ERR_INVALID_STATE;
    }

    // Align rotation_position_ with the page actually shown, or the indicator lies.
    for (size_t i = 0; i < rotation_count_; ++i) {
        if (rotation_[i] == static_cast<size_t>(target)) {
            rotation_position_ = i;
            break;
        }
    }
    showEntry(target, /*animate=*/false);

    ESP_LOGI(kTag, "%u pages built, %u in rotation, showing '%s'",
             static_cast<unsigned>(entry_count_), static_cast<unsigned>(rotation_count_),
             entries_[target].plugin->id());
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Rotation bookkeeping
// ---------------------------------------------------------------------------------------

int PageManager::findIndex(const char* id) const {
    if (id == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].plugin != nullptr && std::strcmp(entries_[i].plugin->id(), id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void PageManager::rebuildRotation() {
    // Remember which page is showing so its position survives a re-order.
    const int showing = (overlay_index_ >= 0) ? -1 : visible_index_;

    rotation_count_ = 0;
    for (size_t o = 0; o < order_count_; ++o) {
        const size_t index = order_[o];
        if (index >= entry_count_) {
            continue;
        }
        const Entry& entry = entries_[index];
        if (entry.in_rotation && entry.enabled) {
            rotation_[rotation_count_++] = index;
        }
    }

    if (showing >= 0) {
        for (size_t i = 0; i < rotation_count_; ++i) {
            if (rotation_[i] == static_cast<size_t>(showing)) {
                rotation_position_ = i;
                break;
            }
        }
    }
    if (rotation_position_ >= rotation_count_) {
        rotation_position_ = 0;
    }

    rebuildIndicators();
}

void PageManager::rebuildIndicators() {
    if (indicator_ == nullptr) {
        return;
    }
    lv_obj_clean(indicator_);

    // A single page needs no indicator, and two dots for a two-page rotation is noise.
    if (rotation_count_ < 2 || overlay_index_ >= 0) {
        lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < rotation_count_; ++i) {
        const bool active = (i == rotation_position_);
        lv_obj_t* dot = lv_obj_create(indicator_);
        theme::makePlain(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        // The active marker is a pill rather than a larger circle: it reads as "you are here"
        // without adding a second visual weight to the page.
        lv_obj_set_size(dot, active ? theme::kPageDotActiveWidth : theme::kPageDotSize,
                        theme::kPageDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, active ? theme::accent() : theme::textSecondary(),
                                  LV_PART_MAIN);
    }
}

void PageManager::showEntry(int index, bool animate) {
    if (index < 0 || static_cast<size_t>(index) >= entry_count_) {
        return;
    }
    if (index == visible_index_) {
        return;
    }

    if (visible_index_ >= 0) {
        Entry& previous = entries_[visible_index_];
        if (previous.page != nullptr) {
            lv_obj_add_flag(previous.page, LV_OBJ_FLAG_HIDDEN);
        }
        previous.plugin->onHide();
    }

    Entry& entry = entries_[index];
    visible_index_ = index;
    if (entry.page != nullptr) {
        lv_obj_remove_flag(entry.page, LV_OBJ_FLAG_HIDDEN);
        if (animate) {
            lv_obj_fade_in(entry.page, kTransitionMs, 0);
        } else {
            lv_obj_set_style_opa(entry.page, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
    entry.plugin->onShow();

    rebuildIndicators();
}

// ---------------------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------------------

void PageManager::next() {
    if (overlay_index_ >= 0 || rotation_count_ == 0) {
        return;
    }
    rotation_position_ = (rotation_position_ + 1) % rotation_count_;
    showEntry(static_cast<int>(rotation_[rotation_position_]), /*animate=*/true);
}

void PageManager::previous() {
    if (overlay_index_ >= 0 || rotation_count_ == 0) {
        return;
    }
    rotation_position_ = (rotation_position_ + rotation_count_ - 1) % rotation_count_;
    showEntry(static_cast<int>(rotation_[rotation_position_]), /*animate=*/true);
}

void PageManager::showById(const char* id) {
    const int index = findIndex(id);
    if (index < 0 || !entries_[index].enabled) {
        return;
    }
    if (!entries_[index].in_rotation) {
        openOverlay(id);
        return;
    }
    if (overlay_index_ >= 0) {
        closeOverlay();
    }
    for (size_t i = 0; i < rotation_count_; ++i) {
        if (rotation_[i] == static_cast<size_t>(index)) {
            rotation_position_ = i;
            break;
        }
    }
    showEntry(index, /*animate=*/true);
}

void PageManager::openOverlay(const char* id) {
    const int index = findIndex(id);
    if (index < 0 || overlay_index_ >= 0) {
        return;
    }
    position_before_overlay_ = rotation_position_;
    overlay_index_ = index;
    // Suppress long-press while the overlay is up, so the gesture that opened Settings cannot
    // immediately re-trigger inside it.
    gestures_.setLongPressEnabled(false);
    showEntry(index, /*animate=*/true);
    rebuildIndicators();
    ESP_LOGI(kTag, "overlay '%s' opened", entries_[index].plugin->id());
}

void PageManager::closeOverlay() {
    if (overlay_index_ < 0) {
        return;
    }
    overlay_index_ = -1;
    gestures_.setLongPressEnabled(true);
    rotation_position_ =
        (position_before_overlay_ < rotation_count_) ? position_before_overlay_ : 0;
    if (rotation_count_ > 0) {
        showEntry(static_cast<int>(rotation_[rotation_position_]), /*animate=*/true);
    }
    rebuildIndicators();
}

void PageManager::requestRefresh() {
    if (visible_index_ < 0) {
        return;
    }
    Entry& entry = entries_[visible_index_];
    entry.last_refresh_tick = lv_tick_get();
    entry.ever_refreshed = true;
    entry.plugin->refresh(/*force=*/true);
}

// ---------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------

void PageManager::setEnabled(const char* id, bool enabled) {
    const int index = findIndex(id);
    if (index < 0) {
        return;
    }
    // Never re-enable a plugin that disabled itself: it did so because it cannot work (missing
    // token, unsupported config), and forcing it into the rotation would just show an error.
    if (enabled && entries_[index].plugin->state() == DataState::Disabled) {
        ESP_LOGW(kTag, "'%s' reports itself unusable; leaving it out of the rotation", id);
        return;
    }
    if (entries_[index].enabled == enabled) {
        return;
    }
    entries_[index].enabled = enabled;

    // If the page being disabled is on screen, move off it before it disappears from the
    // rotation, otherwise the user is left looking at a page that no longer exists in the order.
    if (!enabled && visible_index_ == index) {
        rebuildRotation();
        if (rotation_count_ > 0) {
            rotation_position_ %= rotation_count_;
            showEntry(static_cast<int>(rotation_[rotation_position_]), /*animate=*/true);
        }
    } else {
        rebuildRotation();
    }
}

bool PageManager::isEnabled(const char* id) const {
    const int index = findIndex(id);
    return index >= 0 && entries_[index].enabled;
}

void PageManager::setOrder(const char* const* ids, size_t count) {
    if (ids == nullptr) {
        return;
    }
    bool placed[kMaxPlugins] = {};
    size_t new_count = 0;
    size_t new_order[kMaxPlugins] = {};

    for (size_t i = 0; i < count && new_count < kMaxPlugins; ++i) {
        const int index = findIndex(ids[i]);
        // Unknown ids are skipped rather than rejected: a saved order from an older firmware
        // may name a page that no longer exists, and that must not break page ordering.
        if (index < 0 || placed[index]) {
            continue;
        }
        placed[index] = true;
        new_order[new_count++] = static_cast<size_t>(index);
    }

    // Anything the saved order did not mention is appended in registration order, so a NEW page
    // added by a firmware update appears rather than silently vanishing.
    for (size_t i = 0; i < entry_count_ && new_count < kMaxPlugins; ++i) {
        if (!placed[i]) {
            new_order[new_count++] = i;
        }
    }

    std::memcpy(order_, new_order, sizeof(order_));
    order_count_ = new_count;
    rebuildRotation();
}

void PageManager::setOnline(bool online) {
    // Called from the Wi-Fi event task. Only touch atomics here; the work happens on the next
    // tick, on the LVGL thread, where calling into plugins is safe.
    online_.store(online, std::memory_order_relaxed);
    online_dirty_.store(true, std::memory_order_release);
}

void PageManager::reloadPageConfiguration() {
    if (config_loader_) {
        config_loader_();
    }
    rebuildRotation();
}

DashboardPlugin* PageManager::currentPlugin() const {
    if (visible_index_ < 0) {
        return nullptr;
    }
    return entries_[visible_index_].plugin;
}

// ---------------------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------------------

void PageManager::tickCb(lv_timer_t* timer) {
    auto* self = static_cast<PageManager*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->onTick();
    }
}

void PageManager::onTick() {
    applyPendingOnlineState();

    for (size_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].plugin != nullptr) {
            entries_[i].plugin->tick();
        }
    }

    scheduleRefreshes();
}

void PageManager::applyPendingOnlineState() {
    if (!online_dirty_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const bool online = online_.load(std::memory_order_relaxed);
    ESP_LOGI(kTag, "network %s", online ? "online" : "offline");
    for (size_t i = 0; i < entry_count_; ++i) {
        if (entries_[i].plugin != nullptr) {
            entries_[i].plugin->onNetworkChanged(online);
        }
    }
}

void PageManager::scheduleRefreshes() {
    // First pass: give one not-yet-refreshed plugin a turn per tick. Staggering the initial
    // fetches by 250 ms each stops five TLS handshakes from starting simultaneously at boot,
    // which would spike heap use and delay the first paint.
    for (size_t i = 0; i < entry_count_; ++i) {
        Entry& entry = entries_[i];
        if (!entry.enabled || entry.ever_refreshed || entry.plugin == nullptr) {
            continue;
        }
        if (entry.plugin->state() == DataState::Disabled) {
            continue;
        }
        // A zero interval means "never scheduled" — placeholder pages and any page whose content
        // is entirely local. Refreshing it once anyway would push it to Ok and print a
        // meaningless "Updated just now" in its footer.
        if (entry.plugin->refreshIntervalMs() == 0) {
            continue;
        }
        entry.ever_refreshed = true;
        entry.last_refresh_tick = lv_tick_get();
        entry.plugin->refresh(/*force=*/false);
        return;
    }

    // Steady state: refresh anything whose interval has elapsed.
    for (size_t i = 0; i < entry_count_; ++i) {
        Entry& entry = entries_[i];
        if (!entry.enabled || entry.plugin == nullptr) {
            continue;
        }
        if (entry.plugin->state() == DataState::Disabled) {
            continue;
        }
        const bool visible = (static_cast<int>(i) == visible_index_);
        if (!visible && !entry.plugin->refreshWhenHidden()) {
            continue;
        }
        const uint32_t interval = entry.plugin->refreshIntervalMs();
        if (interval == 0) {
            continue;
        }
        // lv_tick_elaps() rather than subtraction: the tick counter wraps roughly every
        // 49 days, and a naive comparison would then stop refreshing until the next wrap.
        if (lv_tick_elaps(entry.last_refresh_tick) >= interval) {
            entry.last_refresh_tick = lv_tick_get();
            entry.plugin->refresh(/*force=*/false);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Gestures
// ---------------------------------------------------------------------------------------

void PageManager::onGesture(Gesture gesture) {
    switch (gesture) {
        case Gesture::SwipeLeft:
            next();
            break;
        case Gesture::SwipeRight:
            previous();
            break;
        case Gesture::SwipeDown:
            requestRefresh();
            break;
        case Gesture::LongPress:
            if (overlay_index_ >= 0) {
                closeOverlay();
            } else if (overlay_page_id_ != nullptr) {
                openOverlay(overlay_page_id_);
            }
            break;
        case Gesture::SwipeUp:
            // Reserved. Deliberately inert rather than mapped to something surprising.
            break;
    }
}

}  // namespace dashboard
