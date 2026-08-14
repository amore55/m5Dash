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

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "app_config.hpp"
#include "version.hpp"

#include "dashboard/network_indicator.hpp"
#include "dashboard/net/web_server.hpp"
#include "dashboard/net/time_sync.hpp"
#include "dashboard/net/wifi_manager.hpp"
#include "dashboard/page_manager.hpp"
#include "dashboard/storage/cache_store.hpp"
#include "dashboard/storage/secret_store.hpp"
#include "dashboard/storage/fs.hpp"
#include "dashboard/storage/settings_store.hpp"
#include "dashboard/storage/task_store.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"
#include "tab5_board/board.hpp"

#include "placeholder_plugin.hpp"
#include "plugins/clock_plugin.hpp"
#include "plugins/elizabeth_plugin.hpp"
#include "plugins/weather_plugin.hpp"

namespace {

constexpr const char* kTag = "app";

/// How often the liveness/heap report is printed. 30 s is frequent enough to spot a reboot
/// loop within one report, and rare enough not to clutter the log.
constexpr uint64_t kHealthReportPeriodUs = 30ULL * 1000ULL * 1000ULL;

// Statically allocated. Plugins live for the lifetime of the application, so there is no reason
// to put them on the heap and every reason not to: their footprint is then visible in the
// linker map rather than being a runtime surprise.
plugins::ClockPlugin g_clock;

plugins::WeatherPlugin g_weather;

plugins::ElizabethPlugin g_elizabeth;

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
dashboard::net::WifiManager g_wifi;
dashboard::net::WebServer g_web;
dashboard::net::TimeSync g_time;

/// Set by the portal when new credentials land, cleared by the supervisor when it acts on them.
///
/// A flag rather than a direct call, because the two live on different tasks: the HTTP handler
/// must return a response promptly, and having it drive the radio would also let the supervisor
/// tear the portal down from underneath the very request that configured it.
std::atomic<bool> g_credentials_changed{false};

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

    // The weather page holds the location itself rather than reading g_settings, so that the only
    // thread-shared copy of it is behind the plugin's own mutex. A changed coordinate refetches.
    g_weather.setLocation(g_settings.latitude, g_settings.longitude,
                          g_settings.weather_label.c_str());

    // These control how OFTEN the line is polled, not which way the departure board faces — that
    // turns round at midday. See elizabeth_plugin.hpp.
    g_elizabeth.setCommuteWindows(g_settings.commute_morning_start_minutes,
                                  g_settings.commute_morning_end_minutes,
                                  g_settings.commute_evening_start_minutes,
                                  g_settings.commute_evening_end_minutes);
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
///
/// INTERNAL and DMA are reported separately, and that is the important part. The total is 33 MB of
/// PSRAM and tells you almost nothing: the resources that actually run out on this board are the
/// ~500 KB of internal SRAM and the DMA-capable subset of it, which is what the TLS accelerators
/// and the esp_hosted SDIO driver compete for. A log line saying "free heap: 31,754,044 bytes"
/// was printed moments before this firmware died of memory exhaustion.
void healthTimerCb(void*) {
    ESP_LOGI(kTag,
             "health: uptime %llu s, heap total %" PRIu32 " B (min %" PRIu32
             " B), internal %u B (min %u B), dma %u B",
             esp_timer_get_time() / 1000000ULL, esp_get_free_heap_size(),
             esp_get_minimum_free_heap_size(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)));

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

// ---------------------------------------------------------------------------------------
// Wi-Fi supervisor
// ---------------------------------------------------------------------------------------
//
// WifiManager handles one connection attempt and the backoff behind it. What it deliberately
// does not decide is when to give up on the stored credentials and ask the user for new ones —
// that needs the settings, the secret store and the portal, none of which it knows about.
//
// Runs as its own task rather than on the health timer because everything it calls goes to the
// ESP32-C6 over SDIO and blocks: a scan takes a couple of seconds, and an esp_timer callback
// that blocks for that long stalls every other timer in the system.

/// Write the freshly-synced system time back to the battery-backed RTC.
///
/// This is the other half of the boot-time restoreSystemTime(): without it the RTC would keep
/// whatever it was last told and drift, so a boot with no network would show a slowly worsening
/// lie rather than the right time.
void persistTimeToRtc() {
    auto& rtc = tab5::Board::instance().rtc();
    if (!rtc.attached()) {
        return;
    }
    const esp_err_t err = rtc.persistSystemTime();
    if (err != ESP_OK) {
        // Not fatal in any sense that matters: the system clock is correct either way, and the
        // only cost is a less accurate starting point after the next power cut.
        ESP_LOGW(kTag, "could not write the synced time to the RTC: %s", esp_err_to_name(err));
        return;
    }
    // Sized for the compiler's worst case, not the realistic one: every field is an int, so
    // -Werror=format-truncation reasons about six 11-character numbers rather than a date.
    char stamp[80];
    const std::tm local = dashboard::timeutil::localNow();
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    ESP_LOGI(kTag, "clock synced and written to the RTC: %s local", stamp);
}

/// Translate the radio's state into what the header icon should show.
///
/// The mapping lives here rather than in dashboard_core because this is the only place that knows
/// both the WifiState and the RSSI behind it. Signal strength is only meaningful once connected;
/// every other state is about what the device is doing, not how well.
void publishNetworkIndicator() {
    using dashboard::NetworkIndicator;
    const dashboard::net::WifiState state = g_wifi.state();

    // Checked before Connected, because in AP+STA both can be true and the portal is the more
    // useful thing to report: it is the one the user might still be looking at.
    if (g_wifi.apActive()) {
        dashboard::setNetworkIndicator(NetworkIndicator::SetupPortal);
        return;
    }
    switch (state) {
        case dashboard::net::WifiState::Connected:
            dashboard::setNetworkIndicator(dashboard::indicatorForRssi(g_wifi.rssi()));
            return;
        case dashboard::net::WifiState::Connecting:
            dashboard::setNetworkIndicator(NetworkIndicator::Connecting);
            return;
        case dashboard::net::WifiState::Idle:
        case dashboard::net::WifiState::Disconnected:
        case dashboard::net::WifiState::AccessPoint:
            break;
    }
    dashboard::setNetworkIndicator(NetworkIndicator::Offline);
}

/// Try the stored credentials once. Safe with the portal up — see WifiManager::connect().
void attemptStoredConnect(const char* why) {
    if (!g_settings.provisioned()) {
        return;
    }
    ESP_LOGI(kTag, "wifi supervisor: %s", why);
    dashboard::storage::ScopedSecret password;
    password.load(dashboard::storage::Secret::WifiPassword);
    g_wifi.connect(g_settings.wifi_ssid.c_str(), password.c_str());
}

/// Persist credentials submitted through the portal. Runs on the HTTP server task.
///
/// The passphrase is written first: if that fails there is no point recording an SSID we have no
/// key for, which would leave the device retrying a network it cannot authenticate to.
esp_err_t storeSubmittedCredentials(const char* ssid, const char* password) {
    esp_err_t err =
        dashboard::storage::SecretStore::set(dashboard::storage::Secret::WifiPassword, password);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "saving the Wi-Fi passphrase failed: %s", esp_err_to_name(err));
        return err;
    }

    g_settings.wifi_ssid.assign(ssid);
    err = g_settings_store.save(g_settings);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "saving settings failed: %s", esp_err_to_name(err));
        return err;
    }

    // The SSID is safe to log. The passphrase is not, and never reaches a log call.
    ESP_LOGI(kTag, "stored Wi-Fi credentials for '%s'", ssid);

    // Hand the connection attempt to the supervisor rather than making it here — see
    // g_credentials_changed for why this task must not drive the radio.
    g_credentials_changed.store(true);
    return ESP_OK;
}

/// Read side of the web server's settings API. Runs on the HTTP server task.
void readSettingsSnapshot(dashboard::storage::Settings& out) { out = g_settings; }

/// Apply and persist settings edited through the web page. Runs on the HTTP server task.
esp_err_t applyEditedSettings(const dashboard::storage::Settings& incoming) {
    const dashboard::storage::Settings previous = g_settings;
    g_settings = incoming;

    const esp_err_t err = g_settings_store.save(g_settings);
    if (err != ESP_OK) {
        // Put the old values back rather than running settings the device will forget: a
        // dashboard that behaves one way now and another after a reboot is worse than one that
        // simply refused the change and said so.
        g_settings = previous;
        ESP_LOGE(kTag, "settings write failed, reverted: %s", esp_err_to_name(err));
        return err;
    }

    {
        // This task is the HTTP server's, and applySettings() reaches into the clock plugin's
        // LVGL objects to switch face and seconds. Everything touching the UI takes the lock.
        tab5::LvglLock lock;
        applySettings(tab5::Board::instance());
    }
    return ESP_OK;
}

/// Start the configuration web server. Idempotent, and deliberately never stopped afterwards.
void startWebServer() {
    dashboard::net::WebServer::Callbacks callbacks;
    callbacks.on_wifi = &storeSubmittedCredentials;
    callbacks.read_settings = &readSettingsSnapshot;
    callbacks.write_settings = &applyEditedSettings;
    g_web.start(g_wifi, callbacks);
}

/// Advertise the device as <kMdnsHostname>.local, so the settings page can be reached without
/// anyone having to find out what address the router handed out this week.
void startMdns() {
    static bool started = false;
    if (started) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(kTag, "mDNS failed to start; the settings page is reachable by IP only");
        return;
    }
    mdns_hostname_set(dash::cfg::kMdnsHostname);
    mdns_instance_name_set(dash::kProductName);
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    started = true;

    char ip[16];
    g_wifi.ipAddress(ip, sizeof(ip));
    ESP_LOGI(kTag, "settings available at http://%s.local (or http://%s)",
             dash::cfg::kMdnsHostname, ip);
}

void raiseSetupPortal(const char* why) {
    if (g_wifi.apActive()) {
        return;
    }
    // Log text stays ASCII: the serial console mangles multi-byte characters.
    ESP_LOGW(kTag, "wifi supervisor: raising the setup portal - %s", why);
    if (g_wifi.startAccessPoint(dash::cfg::kSetupApSsid) != ESP_OK) {
        ESP_LOGE(kTag, "wifi supervisor: the setup access point failed to start");
        return;
    }

    // The web server is already running and binds every interface, so the access point is the
    // only new thing here.
    char ip[16];
    g_wifi.apIpAddress(ip, sizeof(ip));
    ESP_LOGW(kTag, "join '%s' and browse to http://%s to finish setup", dash::cfg::kSetupApSsid,
             ip);
}

void closeSetupPortal(const char* why) {
    if (!g_wifi.apActive()) {
        return;
    }
    ESP_LOGI(kTag, "wifi supervisor: closing the setup access point - %s", why);
    // Only the access point goes away. The server keeps running on the station interface, which
    // is where the settings page is wanted most of the time.
    g_wifi.stopAccessPoint();
}

void wifiSupervisorTask(void*) {
    // Time spent unable to find the configured network, and time the portal has been up without
    // a station retry. Both counted in supervisor ticks rather than wall clock: this task is the
    // only thing that acts on them, so its own cadence is the honest unit.
    uint32_t absent_ms = 0;
    uint32_t portal_idle_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(dash::cfg::kWifiSupervisorPeriodMs));

        // Refreshed every tick, not only on state changes: signal strength drifts while the
        // state stays Connected, and a strength icon that only moves on connect would be worse
        // than none at all.
        publishNetworkIndicator();

        const dashboard::net::WifiState state = g_wifi.state();

        // Connected. Tear the portal down if it is still up — its whole purpose is served.
        if (state == dashboard::net::WifiState::Connected) {
            absent_ms = 0;
            portal_idle_ms = 0;
            g_credentials_changed.store(false);
            closeSetupPortal("connected");
            startMdns();

            // Started here rather than at boot: SNTP with no route only produces failures that
            // say nothing about the actual problem. begin() is idempotent, so calling it on every
            // tick while connected is free after the first.
            g_time.begin(dash::cfg::kDefaultNtpServer1, dash::cfg::kDefaultNtpServer2);

            // The RTC write happens on this task, not in SNTP's callback, which runs on lwip's
            // stack — see TimeSync::consumeSyncEvent().
            if (g_time.consumeSyncEvent()) {
                persistTimeToRtc();
            }
            continue;
        }

        // Portal up, station not connected. Retry the stored credentials occasionally: the portal
        // stays reachable throughout, so this costs the user nothing and recovers the device on
        // its own if the network it was waiting for comes back.
        if (g_wifi.apActive()) {
            // Someone just submitted the form. Try it at once rather than making them wait out
            // the slow retry cadence below, which would look like the portal had ignored them.
            if (g_credentials_changed.exchange(false)) {
                portal_idle_ms = 0;
                attemptStoredConnect("credentials were just submitted through the portal");
                continue;
            }
            portal_idle_ms += dash::cfg::kWifiSupervisorPeriodMs;
            if (portal_idle_ms >= dash::cfg::kWifiPortalRetryPeriodMs) {
                portal_idle_ms = 0;
                attemptStoredConnect("retrying stored credentials with the portal still up");
            }
            continue;
        }

        // Nothing configured and no portal: first run, or settings were wiped. Nothing to retry.
        if (!g_settings.provisioned()) {
            raiseSetupPortal("no Wi-Fi credentials are stored");
            continue;
        }

        // The access point is rejecting our key. More attempts will not change its mind, so ask
        // the user instead of retrying until the heat death of the universe.
        if (g_wifi.lastFailure() == dashboard::net::WifiFailure::Credentials &&
            g_wifi.retryCount() >= dash::cfg::kWifiAuthFailuresBeforePortal) {
            raiseSetupPortal("the stored Wi-Fi password was rejected");
            continue;
        }

        // Otherwise the network is merely unreachable. Keep waiting — WifiManager is already
        // retrying with backoff — but not forever.
        absent_ms += dash::cfg::kWifiSupervisorPeriodMs;
        if (absent_ms >= dash::cfg::kWifiAbsentBeforePortalMs) {
            absent_ms = 0;
            raiseSetupPortal("the configured network has been unreachable for a long time");
        }
    }
}

void startWifiSupervisor() {
    // 4 kB: the deepest thing on this stack is an esp_wifi call marshalled over the esp-hosted
    // RPC layer, plus a ScopedSecret buffer.
    if (xTaskCreate(&wifiSupervisorTask, "wifi_sup", 4096, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "wifi supervisor task failed to start; no automatic setup-portal recovery");
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

/// Point cJSON's allocator at PSRAM. Must run before anything parses a response.
///
/// Every API this device reads is JSON, and parsing builds one small allocation per node — a few
/// hundred of them for an 11 KB TfL arrivals document. CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is
/// 16384, meaning malloc() serves anything smaller than 16 KB from INTERNAL SRAM, so every one of
/// those nodes lands in the scarcest memory on the board.
///
/// HONEST ACCOUNTING: this was expected to be the main remaining consumer of internal SRAM and it
/// was not. Measured across a boot with both plugins fetching, it moved the internal low-water mark
/// from 22.0 KB to 23.4 KB — real, but far less than predicted. The bulk of the transient demand is
/// the TLS session itself, not the parse. Kept because it is the right home for this workload and
/// because 1.4 KB of the scarcest memory is still 1.4 KB.
///
/// Lowering ALWAYSINTERNAL instead would have pushed every small allocation in lwIP, mbedtls and
/// the drivers into slow PSRAM as well. This moves exactly the workload that should move: large,
/// transient, and utterly insensitive to memory latency.
void useExternalMemoryForJson() {
    cJSON_Hooks hooks = {};
    hooks.malloc_fn = [](size_t size) -> void* {
        void* block = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        // Fall back rather than fail a parse: an exhausted PSRAM pool should degrade, not break
        // every page at once.
        return (block != nullptr) ? block : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    };
    hooks.free_fn = [](void* block) { heap_caps_free(block); };
    cJSON_InitHooks(&hooks);
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

    // Before any plugin can parse a cached response at start-up.
    useExternalMemoryForJson();

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

    // ---- Wi-Fi ------------------------------------------------------------------------
    //
    // Deliberately last: the dashboard is useful without it, and bringing the radio up after
    // the UI means a slow or failing ESP32-C6 delays nothing the user can see.
    //
    // Not fatal either. A device that cannot reach its radio still shows the clock, and the
    // setup portal is the recovery path for missing credentials.
    if (g_wifi.begin() == ESP_OK) {
        g_wifi.setStateCallback([](dashboard::net::WifiState state, bool online) {
            ESP_LOGI(kTag, "network state: %s", dashboard::net::toString(state));
            // Cheap and atomic, so it is safe to do straight from the event task. The header
            // picks it up on the next LVGL tick.
            publishNetworkIndicator();
            // Runs on the system event task. PageManager::setOnline() is explicitly safe to
            // call from another thread and defers the work to the LVGL tick.
            g_pages.setOnline(online);
        });

        if (g_settings.provisioned()) {
            attemptStoredConnect("connecting with stored credentials");
        } else {
            // No credentials yet. A scan is the cheapest end-to-end proof that the ESP32-C6
            // link works, and it logs what is in range before the portal goes up.
            ESP_LOGW(kTag, "no Wi-Fi credentials stored; first-run setup is required");
            g_wifi.scanAndLog();
        }

        // Started before the supervisor and never stopped: it answers on the setup access point
        // and on the home network alike, and the settings page is wanted far more often than the
        // first-run portal ever is.
        startWebServer();

        // From here on the supervisor owns the decision to raise or close the setup portal —
        // including the first-run case above, so that one code path handles it rather than two.
        startWifiSupervisor();
    } else {
        ESP_LOGE(kTag, "Wi-Fi unavailable; running offline");
    }

    ESP_LOGI(kTag, "dashboard running; free heap: %" PRIu32 " B total, %u B internal, %u B DMA",
             esp_get_free_heap_size(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)));

    // app_main returns here. LVGL runs on the esp_lvgl_port task, plugin work runs on each
    // plugin's worker task, and PageManager's single lv_timer drives everything else. There is
    // deliberately no busy loop in this function.
}
