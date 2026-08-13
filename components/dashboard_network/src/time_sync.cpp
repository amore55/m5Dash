#include "dashboard/net/time_sync.hpp"

#include <atomic>
#include <cinttypes>

#include "esp_log.h"
#include "esp_sntp.h"

#include "app_config.hpp"

namespace dashboard::net {
namespace {

constexpr const char* kTag = "time";

// File scope rather than members: SNTP's notification callback is a bare function pointer with no
// user-data argument, so there is nowhere to hang an instance. There is one system clock, so one
// set of state is not a compromise.
std::atomic<bool> g_sync_pending{false};
std::atomic<bool> g_ever_synced{false};
std::atomic<int64_t> g_last_sync_utc{0};

}  // namespace

void TimeSync::onSynced(struct timeval* tv) {
    const int64_t utc = (tv != nullptr) ? static_cast<int64_t>(tv->tv_sec) : 0;
    g_last_sync_utc.store(utc, std::memory_order_relaxed);
    g_ever_synced.store(true, std::memory_order_relaxed);
    g_sync_pending.store(true, std::memory_order_release);

    // lwip task. Records and logs, nothing more — see consumeSyncEvent().
    ESP_LOGI(kTag, "SNTP sync complete, UTC epoch %" PRId64, utc);
}

esp_err_t TimeSync::begin(const char* primary, const char* secondary) {
    if (started_) {
        return ESP_OK;
    }
    if (primary == nullptr || primary[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (esp_sntp_enabled()) {
        // Something else already started it. Adopt that rather than reinitialising underneath it.
        started_ = true;
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, primary);
    if (secondary != nullptr && secondary[0] != '\0') {
        esp_sntp_setservername(1, secondary);
    }
    sntp_set_time_sync_notification_cb(&TimeSync::onSynced);

    // Overrides the compile-time CONFIG_LWIP_SNTP_UPDATE_DELAY at runtime. A desk clock does not
    // need the network every hour; the RTC covers the gap between syncs.
    sntp_set_sync_interval(dash::cfg::kNtpResyncMs);

    esp_sntp_init();
    started_ = true;

    ESP_LOGI(kTag, "SNTP started (%s, %s), resync every %" PRIu32 " min", primary,
             (secondary != nullptr && secondary[0] != '\0') ? secondary : "none",
             dash::cfg::kNtpResyncMs / 60000u);
    return ESP_OK;
}

bool TimeSync::synced() const { return g_ever_synced.load(std::memory_order_relaxed); }

bool TimeSync::consumeSyncEvent() {
    return g_sync_pending.exchange(false, std::memory_order_acq_rel);
}

std::time_t TimeSync::lastSyncUtc() const {
    return static_cast<std::time_t>(g_last_sync_utc.load(std::memory_order_relaxed));
}

}  // namespace dashboard::net
