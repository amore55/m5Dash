// Owns the page rotation, navigation, scheduling and the single UI tick.
//
// KEY DESIGN CHOICE — pages are created ONCE and never destroyed. Navigation toggles
// LV_OBJ_FLAG_HIDDEN. This costs memory (six full-screen widget trees resident, comfortably
// affordable with 32 MB of PSRAM) and buys the guarantee the brief asks for: "avoid memory
// leaks when pages are shown repeatedly" becomes structurally impossible rather than something
// each plugin has to get right.
//
// There is exactly ONE lv_timer for the whole application. It drives every plugin's tick() and
// decides when each is due a refresh. Plugins do not own timers, so there is one place to look
// when the UI stutters, and refresh scheduling cannot drift out of sync between pages.
//
// Threading: everything here runs on the LVGL thread except setOnline(), which is explicitly
// safe to call from the Wi-Fi event task and defers its work to the next tick.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "lvgl.h"

#include "dashboard/gesture_detector.hpp"
#include "dashboard/plugin.hpp"

namespace dashboard {

class PageManager : public PageHost {
  public:
    /// Bounded: registration is static, so a fixed array avoids any dynamic allocation and
    /// makes the memory cost of the page system knowable at compile time.
    static constexpr size_t kMaxPlugins = 10;

    /// Prepare the screen, apply the global theme and start gesture detection.
    esp_err_t begin(lv_display_t* display);

    /// Register a plugin. `in_rotation` false means it is reachable only as an overlay
    /// (Settings). Does not call into the plugin — initialise() is the caller's job, so a
    /// plugin that fails to initialise can still be registered and shown as Disabled.
    esp_err_t add(DashboardPlugin* plugin, bool in_rotation);

    /// Create every page's widget tree and show the default page.
    /// Call after all plugins are added and initialised.
    esp_err_t startPages(const char* default_page_id);

    /// The id treated as the Settings overlay, opened by long press. Optional.
    void setOverlayPageId(const char* id) { overlay_page_id_ = id; }

    /// Supplied by the application: re-reads page order / enabled flags from settings.
    /// Kept as a callback so dashboard_core does not depend on dashboard_storage.
    void setConfigurationLoader(std::function<void()> loader) {
        config_loader_ = std::move(loader);
    }

    // ---- navigation -------------------------------------------------------------------
    void next();
    void previous();
    void showById(const char* id) override;
    void openOverlay(const char* id);
    void closeOverlay() override;
    void requestRefresh() override;
    bool overlayOpen() const { return overlay_index_ >= 0; }

    // ---- configuration ----------------------------------------------------------------

    /// Enable/disable a page. A disabled page is removed from the rotation and stops being
    /// scheduled, but keeps its widget tree so re-enabling is instant.
    void setEnabled(const char* id, bool enabled);
    bool isEnabled(const char* id) const;

    /// Re-order the rotation. Ids not present are appended in registration order, and unknown
    /// ids are ignored — so a stale saved order from an older firmware cannot hide a page.
    void setOrder(const char* const* ids, size_t count);

    /// Thread-safe. Applied on the next tick, which then notifies every plugin.
    void setOnline(bool online);
    bool online() const { return online_.load(std::memory_order_relaxed); }

    // ---- PageHost ---------------------------------------------------------------------
    size_t rotationCount() const override { return rotation_count_; }
    size_t rotationIndex() const override { return rotation_position_; }
    void reloadPageConfiguration() override;

    // ---- accessors --------------------------------------------------------------------
    DashboardPlugin* currentPlugin() const;
    GestureDetector& gestures() { return gestures_; }

  private:
    struct Entry {
        DashboardPlugin* plugin = nullptr;
        lv_obj_t* page = nullptr;
        bool in_rotation = false;
        bool enabled = true;
        /// lv_tick at the last scheduled refresh. Compared with lv_tick_elaps(), which is
        /// wraparound-safe — plain subtraction is not, and this counter wraps every ~49 days.
        uint32_t last_refresh_tick = 0;
        bool ever_refreshed = false;
    };

    static void tickCb(lv_timer_t* timer);
    void onTick();
    void onGesture(Gesture gesture);

    int findIndex(const char* id) const;
    void rebuildRotation();
    void rebuildIndicators();
    void showEntry(int index, bool animate);
    void scheduleRefreshes();
    void applyPendingOnlineState();

    Entry entries_[kMaxPlugins];
    size_t entry_count_ = 0;

    /// Indices into entries_, in display order — only enabled, in-rotation pages.
    size_t rotation_[kMaxPlugins] = {};
    size_t rotation_count_ = 0;
    size_t rotation_position_ = 0;

    /// Registration-order preference list used by rebuildRotation(). Holds entry indices.
    size_t order_[kMaxPlugins] = {};
    size_t order_count_ = 0;

    int visible_index_ = -1;
    int overlay_index_ = -1;
    size_t position_before_overlay_ = 0;

    const char* overlay_page_id_ = nullptr;
    std::function<void()> config_loader_;

    lv_obj_t* screen_ = nullptr;
    lv_timer_t* tick_timer_ = nullptr;

    /// The page-position dots. A SINGLE widget on LVGL's top layer, shared by every page,
    /// rather than one per page: PageManager only holds a DashboardPlugin* and cannot reach a
    /// plugin's footer, and one indicator is less to keep in sync than six. Non-clickable, so
    /// touches pass straight through to the page beneath.
    lv_obj_t* indicator_ = nullptr;

    GestureDetector gestures_;

    std::atomic<bool> online_{false};
    std::atomic<bool> online_dirty_{false};
};

}  // namespace dashboard
