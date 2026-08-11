#include "dashboard/net/wifi_manager.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.hpp"
#include "tab5_board/board.hpp"

namespace dashboard::net {
namespace {

constexpr const char* kTag = "wifi";

esp_netif_t* g_sta_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;

/// Retry delay grows with consecutive failures and then holds.
///
/// A device on a desk beside a router that has been unplugged should not spend the evening
/// hammering a scan every two seconds — it wastes power and floods the log. Equally it must
/// recover promptly when the router comes back, so the first few retries are fast.
uint32_t backoffDelayMs(int retry_count) {
    if (retry_count <= dash::cfg::kWifiFastRetries) {
        return 2000;
    }
    const uint32_t delay =
        2000u << static_cast<uint32_t>(retry_count - dash::cfg::kWifiFastRetries);
    return delay > dash::cfg::kWifiRetryBackoffMaxMs ? dash::cfg::kWifiRetryBackoffMaxMs : delay;
}

/// Human-readable disconnect reasons for the ones that actually mean something to a user.
/// Everything else is reported numerically rather than guessed at.
const char* describeDisconnectReason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "wrong password or auth failure";
        case WIFI_REASON_NO_AP_FOUND:
            return "network not found";
        case WIFI_REASON_ASSOC_LEAVE:
            return "disconnected by request";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "lost contact with the access point";
        default:
            return "see reason code";
    }
}

}  // namespace

const char* toString(WifiState state) {
    switch (state) {
        case WifiState::Idle:
            return "idle";
        case WifiState::Connecting:
            return "connecting";
        case WifiState::Connected:
            return "connected";
        case WifiState::Disconnected:
            return "disconnected";
        case WifiState::AccessPoint:
            return "access-point";
    }
    return "?";
}

esp_err_t WifiManager::begin() {
    if (started_) {
        return ESP_OK;
    }

    // THE STEP EVERYONE MISSES. Powers the ESP32-C6 through the board's second IO expander.
    // Without it esp_wifi_init() fails in a way that looks like a software fault.
    ESP_RETURN_ON_ERROR(tab5::Board::instance().enableWifiRail(), kTag, "C6 power rail failed");

    // The C6 needs a moment after power-up before the SDIO link will enumerate.
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "esp_netif_init failed");

    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "event loop failed: %s", esp_err_to_name(loop_err));
        return loop_err;
    }

    g_sta_netif = esp_netif_create_default_wifi_sta();
    if (g_sta_netif == nullptr) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t init_err = esp_wifi_init(&init_cfg);
    if (init_err != ESP_OK) {
        // Most likely cause by far: the C6's esp-hosted slave firmware does not match the host
        // component version. Say so, because the error code alone sends people looking in
        // completely the wrong place.
        ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(init_err));
        ESP_LOGE(kTag,
                 "the ESP32-C6 may need its esp-hosted slave firmware reflashed — "
                 "see docs/FLASHING.md");
        return init_err;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &WifiManager::eventHandler, this,
                                                            nullptr),
                        kTag, "wifi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &WifiManager::eventHandler, this,
                                                            nullptr),
                        kTag, "ip event registration failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "esp_wifi_start failed");

    started_ = true;
    setState(WifiState::Idle);

    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(kTag, "Wi-Fi up via ESP32-C6, MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t WifiManager::connect(const char* ssid, const char* password) {
    if (!started_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == nullptr || ssid[0] == '\0') {
        ESP_LOGW(kTag, "no SSID configured; not connecting");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {};
    std::snprintf(reinterpret_cast<char*>(cfg.sta.ssid), sizeof(cfg.sta.ssid), "%s", ssid);
    if (password != nullptr) {
        std::snprintf(reinterpret_cast<char*>(cfg.sta.password), sizeof(cfg.sta.password), "%s",
                      password);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  // accept whatever the AP offers
    cfg.sta.pmf_cfg.capable = true;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), kTag, "set_config failed");

    retry_count_ = 0;
    setState(WifiState::Connecting);
    // The SSID is fine to log. The password is not, and is never passed to a log call.
    ESP_LOGI(kTag, "connecting to '%s'", ssid);
    return esp_wifi_connect();
}

esp_err_t WifiManager::startAccessPoint(const char* ssid) {
    if (!started_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (g_ap_netif == nullptr) {
            return ESP_FAIL;
        }
    }

    wifi_config_t cfg = {};
    std::snprintf(reinterpret_cast<char*>(cfg.ap.ssid), sizeof(cfg.ap.ssid), "%s", ssid);
    cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid));
    cfg.ap.channel = dash::cfg::kSetupApChannel;
    cfg.ap.max_connection = dash::cfg::kSetupApMaxConnections;
    // Deliberately open. The AP exists because no credentials are known yet; any password would
    // have to be printed on the screen anyway, and it is short-lived with one client.
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), kTag, "AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), kTag, "AP config failed");

    setState(WifiState::AccessPoint);
    ESP_LOGI(kTag, "setup access point '%s' is up on channel %d", ssid, cfg.ap.channel);
    return ESP_OK;
}

void WifiManager::disconnect() {
    if (!started_) {
        return;
    }
    retry_count_ = -1;  // suppress automatic reconnection
    esp_wifi_disconnect();
    setState(WifiState::Idle);
}

void WifiManager::ipAddress(char* out, size_t capacity) const {
    if (out == nullptr || capacity == 0) {
        return;
    }
    esp_netif_ip_info_t info = {};
    if (g_sta_netif == nullptr || esp_netif_get_ip_info(g_sta_netif, &info) != ESP_OK) {
        std::snprintf(out, capacity, "0.0.0.0");
        return;
    }
    std::snprintf(out, capacity, IPSTR, IP2STR(&info.ip));
}

int WifiManager::rssi() const {
    if (state_ != WifiState::Connected) {
        return 0;
    }
    wifi_ap_record_t record = {};
    if (esp_wifi_sta_get_ap_info(&record) != ESP_OK) {
        return 0;
    }
    return record.rssi;
}

esp_err_t WifiManager::scanAndLog(size_t max_results) {
    if (!started_) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(kTag, "scanning for networks (this proves the ESP32-C6 link end to end)...");
    const esp_err_t err = esp_wifi_scan_start(nullptr, true);  // blocking
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        ESP_LOGW(kTag, "scan completed but found no networks");
        return ESP_OK;
    }

    uint16_t wanted = static_cast<uint16_t>(found < max_results ? found : max_results);
    auto* records = static_cast<wifi_ap_record_t*>(calloc(wanted, sizeof(wifi_ap_record_t)));
    if (records == nullptr) {
        esp_wifi_scan_get_ap_records(&wanted, nullptr);  // discard, free driver memory
        return ESP_ERR_NO_MEM;
    }

    esp_wifi_scan_get_ap_records(&wanted, records);
    ESP_LOGI(kTag, "found %u networks (showing %u):", found, wanted);
    for (uint16_t i = 0; i < wanted; ++i) {
        ESP_LOGI(kTag, "  %-32s ch %2d  %4d dBm  %s",
                 reinterpret_cast<const char*>(records[i].ssid), records[i].primary,
                 records[i].rssi,
                 records[i].authmode == WIFI_AUTH_OPEN ? "open" : "secured");
    }
    free(records);
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------

void WifiManager::eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiManager*>(arg);
    if (self == nullptr) {
        return;
    }
    if (base == WIFI_EVENT) {
        self->onWifiEvent(id, data);
    } else if (base == IP_EVENT) {
        self->onIpEvent(id, data);
    }
}

void WifiManager::onWifiEvent(int32_t id, void* data) {
    switch (id) {
        case WIFI_EVENT_STA_START:
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            if (retry_count_ < 0) {
                // Deliberate disconnect; do not fight the user.
                break;
            }
            const auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
            const uint8_t reason = (event != nullptr) ? event->reason : 0;
            setState(WifiState::Disconnected);
            ++retry_count_;
            const uint32_t delay = backoffDelayMs(retry_count_);
            ESP_LOGW(kTag, "disconnected (reason %u: %s); retrying in %" PRIu32 " ms (attempt %d)",
                     reason, describeDisconnectReason(reason), delay, retry_count_);
            scheduleReconnect();
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(kTag, "a device joined the setup access point");
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(kTag, "a device left the setup access point");
            break;

        default:
            break;
    }
}

void WifiManager::onIpEvent(int32_t id, void* data) {
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const auto* event = static_cast<ip_event_got_ip_t*>(data);
    retry_count_ = 0;
    setState(WifiState::Connected);
    if (event != nullptr) {
        ESP_LOGI(kTag, "connected, IP " IPSTR ", RSSI %d dBm", IP2STR(&event->ip_info.ip), rssi());
    }
}

void WifiManager::scheduleReconnect() {
    // Delay on the event task would block every other event, so the wait happens inside the
    // driver's own retry instead: esp_wifi_connect() is re-issued from a short one-shot task.
    // Keeping it this simple avoids another timer to reason about, and the stack is tiny
    // because the task does nothing but sleep and call one function.
    struct Retry {
        WifiManager* self;
        uint32_t delay_ms;
    };
    auto* retry = new (std::nothrow) Retry{this, backoffDelayMs(retry_count_)};
    if (retry == nullptr) {
        return;
    }
    const BaseType_t created = xTaskCreate(
        [](void* arg) {
            auto* r = static_cast<Retry*>(arg);
            vTaskDelay(pdMS_TO_TICKS(r->delay_ms));
            if (r->self->state() != WifiState::Connected && r->self->state() != WifiState::AccessPoint) {
                esp_wifi_connect();
            }
            delete r;
            vTaskDelete(nullptr);
        },
        "wifi_retry", 2560, retry, 3, nullptr);
    if (created != pdPASS) {
        delete retry;
    }
}

void WifiManager::setState(WifiState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (callback_) {
        callback_(state, online());
    }
}

}  // namespace dashboard::net
