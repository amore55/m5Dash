// Tab5 Desk Dashboard — application entry point.
//
// Boot order matters and is not arbitrary:
//
//   1. NVS         — everything else may want to read configuration.
//   2. Board       — display and touch first, so any later failure is VISIBLE rather than
//                    just a serial message nobody is watching.
//   3. Boot screen — proves the panel works and shows the firmware version and git SHA, which
//                    is the first thing you need when a device misbehaves.
//   4. Time        — timezone rules, then restore the system clock from the RTC so the clock
//                    page is useful immediately, before (or entirely without) Wi-Fi.
//   5. Plugins     — initialised individually; one failing is recorded and skipped.
//   6. Pages       — built and shown.
//
// This file is deliberately the minimum needed for a first build. Wi-Fi, storage, OTA and the
// remaining integrations move into AppController as those components land — see
// docs/BACKLOG.md §4.6.

#include <cinttypes>
#include <cstdio>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "app_config.hpp"
#include "version.hpp"

#include "dashboard/page_manager.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"
#include "tab5_board/board.hpp"

#include "placeholder_plugin.hpp"
#include "plugins/clock_plugin.hpp"

namespace {

constexpr const char* kTag = "app";

// Statically allocated. Plugins live for the lifetime of the application, so there is no reason
// to put them on the heap and every reason not to: their footprint is then visible in the
// linker map rather than being a runtime surprise.
plugins::ClockPlugin g_clock;

dash::PlaceholderPlugin g_weather{"weather", "Weather",
                                  "Open-Meteo forecast for a configured latitude and longitude. "
                                  "Current conditions, daily high and low, rain probability and "
                                  "the next few hours."};

dash::PlaceholderPlugin g_elizabeth{"elizabeth", "Elizabeth line",
                                    "Live service status from the TfL Unified API, refreshed "
                                    "more often during configured commute hours."};

dash::PlaceholderPlugin g_todos{"todos", "To-dos",
                                "Tasks captured by sending a message to a private Telegram bot, "
                                "stored on the device and completed by touch."};

dash::PlaceholderPlugin g_claude{"claude", "Claude usage",
                                 "Five-hour and weekly allowance with a locally calculated "
                                 "countdown to reset. Experimental - see docs/CLAUDE_USAGE.md."};

dash::PlaceholderPlugin g_settings{"settings", "Settings",
                                   "Wi-Fi, location, integrations, display and firmware "
                                   "updates. Opened with a long press, outside the page "
                                   "rotation."};

dashboard::PageManager g_pages;

esp_err_t initialiseNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition is full or was written by a newer NVS format. Erasing loses stored
        // settings, but the alternative is a device that cannot boot — and the setup portal
        // exists precisely to recover from this.
        ESP_LOGW(kTag, "NVS unusable (%s); erasing and re-initialising", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/// Minimal splash shown while the rest of the system comes up. Returns the object so it can be
/// removed once the real pages exist. Caller must hold the LVGL lock.
lv_obj_t* showBootScreen() {
    namespace theme = dashboard::theme;

    lv_obj_t* boot = lv_obj_create(lv_screen_active());
    theme::applyPageRoot(boot);
    lv_obj_set_flex_align(boot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(boot, theme::kGapM, LV_PART_MAIN);

    theme::makeLabel(boot, dash::kProductName, theme::fontHero(), theme::textPrimary());

    // ASCII only: LVGL's built-in Montserrat faces carry ASCII plus a few LV_SYMBOL glyphs, so
    // a middle dot or an em dash would render as an empty box.
    char version_line[64];
    std::snprintf(version_line, sizeof(version_line), "v%s   %s", dash::kAppVersion,
                  dash::kGitSha);
    theme::makeLabel(boot, version_line, theme::fontBody(), theme::textMuted());

    return boot;
}

/// Initialise a plugin, logging rather than aborting on failure.
///
/// A plugin that cannot start must never prevent the dashboard from booting — that is the whole
/// point of the plugin isolation the brief asks for. It is still registered, so its page exists
/// and explains itself instead of silently vanishing.
void initialisePlugin(dashboard::DashboardPlugin& plugin) {
    const esp_err_t err = plugin.initialise();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "plugin '%s' did not initialise (%s); it will show as disabled",
                 plugin.id(), esp_err_to_name(err));
    }
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "%s v%s (%s)", dash::kProductName, dash::kAppVersion, dash::kGitSha);
    ESP_LOGI(kTag, "free heap at boot: %" PRIu32 " bytes", esp_get_free_heap_size());

    ESP_ERROR_CHECK(initialiseNvs());

    // Display and touch. This is the one failure that IS fatal: without a panel there is no
    // way to report anything to the user, and no way to run a setup portal they can see.
    tab5::Board& board = tab5::Board::instance();
    ESP_ERROR_CHECK(board.init());

    lv_obj_t* boot_screen = nullptr;
    {
        tab5::LvglLock lock;
        dashboard::theme::applyGlobalTheme(board.display());
        boot_screen = showBootScreen();
    }

    // Timezone rules first, then the RTC. Doing it in this order means the restored time is
    // interpreted with British Summer Time applied from the very first render.
    dashboard::timeutil::setTimezone(dash::cfg::kDefaultTimezone);
    if (board.rtc().attached()) {
        if (board.rtc().restoreSystemTime() != ESP_OK) {
            ESP_LOGW(kTag, "RTC holds no valid time; waiting for network time");
        }
    }

    initialisePlugin(g_clock);
    initialisePlugin(g_weather);
    initialisePlugin(g_elizabeth);
    initialisePlugin(g_todos);
    initialisePlugin(g_claude);
    initialisePlugin(g_settings);

    {
        tab5::LvglLock lock;

        ESP_ERROR_CHECK(g_pages.begin(board.display()));

        // Registration order is the default page order. Settings is registered out of rotation
        // and reached by long press.
        ESP_ERROR_CHECK(g_pages.add(&g_clock, /*in_rotation=*/true));
        ESP_ERROR_CHECK(g_pages.add(&g_weather, /*in_rotation=*/true));
        ESP_ERROR_CHECK(g_pages.add(&g_elizabeth, /*in_rotation=*/true));
        ESP_ERROR_CHECK(g_pages.add(&g_todos, /*in_rotation=*/true));
        ESP_ERROR_CHECK(g_pages.add(&g_claude, /*in_rotation=*/true));
        ESP_ERROR_CHECK(g_pages.add(&g_settings, /*in_rotation=*/false));
        g_pages.setOverlayPageId("settings");

        ESP_ERROR_CHECK(g_pages.startPages("clock"));

        // Remove the splash only once a real page is on screen, so there is never a frame of
        // empty background between the two.
        if (boot_screen != nullptr) {
            lv_obj_delete(boot_screen);
        }
    }

    ESP_LOGI(kTag, "dashboard running; free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    // app_main returns here. LVGL runs on the esp_lvgl_port task, plugin work runs on each
    // plugin's worker task, and PageManager's single lv_timer drives everything else. There is
    // deliberately no busy loop in this function.
}
