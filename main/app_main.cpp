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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app_config.hpp"
#include "version.hpp"

#include "dashboard/page_manager.hpp"
#include "dashboard/storage/cache_store.hpp"
#include "dashboard/storage/fs.hpp"
#include "dashboard/storage/settings_store.hpp"
#include "dashboard/storage/task_store.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"
#include "tab5_board/board.hpp"

#include "placeholder_plugin.hpp"
#include "plugins/clock_plugin.hpp"

namespace {

constexpr const char* kTag = "app";

/// How often the liveness/heap report is printed. 30 s is frequent enough to spot a reboot
/// loop within one report, and rare enough not to clutter the log.
constexpr uint64_t kHealthReportPeriodUs = 30ULL * 1000ULL * 1000ULL;

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

dash::PlaceholderPlugin g_settings_page{"settings", "Settings",
                                   "Wi-Fi, location, integrations, display and firmware "
                                   "updates. Opened with a long press, outside the page "
                                   "rotation."};

dashboard::PageManager g_pages;

dashboard::storage::SettingsStore g_settings_store;
dashboard::storage::Settings g_settings;
dashboard::storage::TaskStore g_tasks;

/// Split a comma-separated list into pointers into `scratch`, which is modified in place.
/// Returns how many entries were produced.
size_t splitCsv(const char* csv, char* scratch, size_t scratch_size, const char** out,
                size_t max_entries) {
    if (csv == nullptr || scratch == nullptr || out == nullptr) {
        return 0;
    }
    std::snprintf(scratch, scratch_size, "%s", csv);
    size_t count = 0;
    char* cursor = scratch;
    while (*cursor != '\0' && count < max_entries) {
        while (*cursor == ' ' || *cursor == ',') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        out[count++] = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            ++cursor;
        }
        if (*cursor == ',') {
            *cursor++ = '\0';
        }
    }
    return count;
}

/// Push the stored page order and enabled flags into PageManager.
///
/// Also registered as PageManager's configuration loader, so the Settings page can change the
/// order and have it take effect without a restart.
void applyPageConfiguration() {
    char scratch[192];
    const char* ids[dashboard::PageManager::kMaxPlugins];
    const size_t count = splitCsv(g_settings.page_order.c_str(), scratch, sizeof(scratch), ids,
                                  dashboard::PageManager::kMaxPlugins);
    if (count > 0) {
        g_pages.setOrder(ids, count);
    }

    // Enabled flags are applied per plugin. An empty enabled_pages list means "all", which
    // Settings::pageEnabled() already handles.
    static const char* const kRotationIds[] = {"clock", "weather", "elizabeth", "todos", "claude"};
    for (const char* id : kRotationIds) {
        g_pages.setEnabled(id, g_settings.pageEnabled(id));
    }
}

/// Apply settings that belong to hardware or to a specific plugin.
void applySettings(tab5::Board& board) {
    dashboard::timeutil::setTimezone(g_settings.timezone.c_str());

    board.backlight().configure(g_settings.brightness_percent,
                                g_settings.night_brightness_percent);

    g_clock.setFace(plugins::clockFaceFromString(g_settings.clock_style.c_str()));
    g_clock.setShowSeconds(g_settings.show_seconds);
}

/// Evaluate the dim schedule and apply it. Cheap, and only touches the panel when the level
/// actually changes, so it is safe to call from a slow periodic timer.
void applyDimSchedule() {
    const int minutes = dashboard::timeutil::localMinutesSinceMidnight();
    if (minutes < 0) {
        return;  // clock not set yet; leave brightness alone
    }
    const bool night = dashboard::timeutil::inTimeWindow(minutes, g_settings.dim_start_minutes,
                                                         g_settings.dim_end_minutes);
    tab5::Board::instance().backlight().applyNightMode(night);
}

/// Report why the device last restarted.
///
/// A silent reboot is the hardest kind of embedded bug to reason about, and the reset reason is
/// the single cheapest clue available — it distinguishes a clean power-on from a panic, a
/// watchdog, or a brownout, before any other diagnosis is attempted. Abnormal causes are logged
/// as warnings so they stand out in a wall of INFO.
void logResetReason() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const char* text = "unknown";
    bool abnormal = true;

    switch (reason) {
        case ESP_RST_POWERON:
            text = "power-on";
            abnormal = false;
            break;
        case ESP_RST_SW:
            text = "software restart (esp_restart)";
            abnormal = false;
            break;
        case ESP_RST_DEEPSLEEP:
            text = "wake from deep sleep";
            abnormal = false;
            break;
        case ESP_RST_USB:
            text = "USB peripheral reset (normal after flashing)";
            abnormal = false;
            break;
        case ESP_RST_PANIC:
            text = "PANIC or unhandled exception";
            break;
        case ESP_RST_INT_WDT:
            text = "INTERRUPT watchdog - interrupts were disabled too long";
            break;
        case ESP_RST_TASK_WDT:
            text = "TASK watchdog - a task did not yield";
            break;
        case ESP_RST_WDT:
            text = "other watchdog (system/RTC)";
            break;
        case ESP_RST_BROWNOUT:
            text = "BROWNOUT - supply voltage dipped; suspect the USB port or cable";
            break;
        case ESP_RST_EXT:
            text = "external reset pin";
            abnormal = false;
            break;
        default:
            break;
    }

    if (abnormal) {
        ESP_LOGW(kTag, "*** last reset: %s (esp_reset_reason=%d) ***", text,
                 static_cast<int>(reason));
        ESP_LOGW(kTag, "*** if a core dump was written, read it with: idf.py coredump-info ***");
    } else {
        ESP_LOGI(kTag, "last reset: %s", text);
    }
}

/// Periodic liveness and memory report.
///
/// Two jobs: it makes an unexplained reboot obvious (the uptime counter goes back to zero), and
/// it makes a slow leak visible long before it becomes a crash. Deliberately infrequent so it
/// does not bury anything else in the log.
void healthTimerCb(void*) {
    ESP_LOGI(kTag, "health: uptime %llu s, free heap %" PRIu32 " B, min free ever %" PRIu32 " B",
             esp_timer_get_time() / 1000000ULL, esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size());

    // The dim schedule rides along on this timer rather than owning one. 30 s granularity is
    // imperceptible for a brightness change that happens twice a day, and one fewer timer is
    // one fewer thing to reason about.
    applyDimSchedule();
}

void startHealthTimer() {
    esp_timer_create_args_t args = {};
    args.callback = &healthTimerCb;
    args.name = "health";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, kHealthReportPeriodUs);
    }
}

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

// ---------------------------------------------------------------------------------------
// TEMPORARY BRING-UP DIAGNOSTIC — remove once the display is confirmed working.
//
// The dashboard theme is deliberately near-black (#0B0D10), which is indistinguishable from
// a dead panel. This paints two unmistakable full-screen colours before any of our UI code
// runs, which splits a blank screen into two very different problems:
//
//   Colours visible  -> panel, backlight, LVGL task, flush path and rotation all work, and
//                       the fault is in the page/theme code.
//   Nothing visible  -> the fault is below that: backlight, DSI, or the flush/rotation path.
//
// Also forces the backlight to 100% for the duration, so brightness cannot be a confounder.
// ---------------------------------------------------------------------------------------
// Off now that the display is confirmed working. Kept rather than deleted because it earned
// its place: during bring-up it was the test that proved the LVGL flush path independently of
// the page/theme code, at a point when a healthy boot log and a black screen looked identical.
// Flip to true if display output ever regresses.
constexpr bool kBootSelfTest = false;
constexpr uint32_t kSelfTestHoldMs = 2000;

void runDisplaySelfTest(tab5::Board& board) {
    if (!kBootSelfTest) {
        return;
    }
    ESP_LOGW(kTag, "display self-test: expect RED then GREEN full screen for %" PRIu32 " ms each",
             kSelfTestHoldMs);

    board.backlight().setTemporary(100);

    lv_obj_t* panel = nullptr;
    {
        tab5::LvglLock lock;
        panel = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
        lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0xFF0000), LV_PART_MAIN);
        ESP_LOGW(kTag, "self-test: screen is %" PRId32 "x%" PRId32,
                 lv_obj_get_width(lv_screen_active()), lv_obj_get_height(lv_screen_active()));
    }
    // Lock released so the LVGL task can actually render before we change anything.
    vTaskDelay(pdMS_TO_TICKS(kSelfTestHoldMs));

    {
        tab5::LvglLock lock;
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x00FF00), LV_PART_MAIN);
    }
    vTaskDelay(pdMS_TO_TICKS(kSelfTestHoldMs));

    {
        tab5::LvglLock lock;
        lv_obj_delete(panel);
    }
    // Put brightness back under the normal day/night policy.
    board.backlight().applyNightMode(false);
    ESP_LOGW(kTag, "display self-test complete");
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
    logResetReason();

    ESP_ERROR_CHECK(initialiseNvs());

    // Display and touch. This is the one failure that IS fatal: without a panel there is no
    // way to report anything to the user, and no way to run a setup portal they can see.
    tab5::Board& board = tab5::Board::instance();
    ESP_ERROR_CHECK(board.init());

    // TEMPORARY: proves the panel/backlight/flush/rotation path independently of our UI code.
    // Delete this call and runDisplaySelfTest() once the display is confirmed working.
    runDisplaySelfTest(board);

    lv_obj_t* boot_screen = nullptr;
    {
        tab5::LvglLock lock;
        dashboard::theme::applyGlobalTheme(board.display());
        boot_screen = showBootScreen();
    }

    // ---- storage --------------------------------------------------------------------
    //
    // The filesystem is not fatal: a device that cannot mount LittleFS loses tasks and the
    // response cache, but the clock, weather and line status all still work. Configuration
    // lives in NVS and is unaffected either way.
    if (dashboard::storage::Fs::mount() == ESP_OK) {
        dashboard::storage::CacheStore::init();
        g_tasks.load();
    } else {
        ESP_LOGW(kTag, "no filesystem: tasks and cached responses are unavailable this boot");
    }

    g_settings_store.load(g_settings);
    applySettings(board);

    // Timezone comes from settings now, then the RTC. Doing it in this order means the restored
    // time is interpreted with British Summer Time applied from the very first render.
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
    initialisePlugin(g_settings_page);

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
        ESP_ERROR_CHECK(g_pages.add(&g_settings_page, /*in_rotation=*/false));
        g_pages.setOverlayPageId("settings");

        // Order and enabled flags come from stored settings; the loader lets the Settings page
        // re-apply them later without a restart.
        g_pages.setConfigurationLoader(&applyPageConfiguration);
        applyPageConfiguration();

        ESP_ERROR_CHECK(g_pages.startPages(g_settings.default_page.c_str()));

        // Remove the splash only once a real page is on screen, so there is never a frame of
        // empty background between the two.
        if (boot_screen != nullptr) {
            lv_obj_delete(boot_screen);
        }
    }

    startHealthTimer();

    ESP_LOGI(kTag, "dashboard running; free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    // app_main returns here. LVGL runs on the esp_lvgl_port task, plugin work runs on each
    // plugin's worker task, and PageManager's single lv_timer drives everything else. There is
    // deliberately no busy loop in this function.
}
