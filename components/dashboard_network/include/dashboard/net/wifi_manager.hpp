// Wi-Fi connection state, for a board whose radio is on another chip.
//
// THE TAB5 SPECIFIC PART
//
// The ESP32-P4 has no radio. Wi-Fi is provided by an ESP32-C6 coprocessor reached over SDIO
// via esp_hosted + esp_wifi_remote, which transparently forwards the ordinary esp_wifi_* API.
// From this file's point of view the API is completely standard — with one exception that is
// very easy to miss and produces a failure that looks like a software bug:
//
//   **bsp_feature_enable(BSP_FEATURE_WIFI, true) must be called before esp_wifi_init().**
//
// That line powers the C6 through the board's second IO expander. Without it the SDIO link
// never comes up. tab5::Board::enableWifiRail() does it, and begin() calls that first.
//
// If esp_wifi_init() reports an RPC or version mismatch, the C6's esp-hosted slave firmware
// does not match the host component and the C6 needs reflashing — see docs/FLASHING.md. The
// SDIO pin map in sdkconfig.defaults is copied from M5Stack's working configuration and is not
// the thing to change.
//
// THREADING
//
// Wi-Fi events arrive on the system event task. The state callback is therefore invoked from
// that task, NOT the LVGL thread — anything touching the UI must marshal, which is exactly what
// PageManager::setOnline() is designed for.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "esp_event_base.h"  // esp_event_base_t, used in the handler signature below

namespace dashboard::net {

enum class WifiState : uint8_t {
    Idle,          ///< Radio up, doing nothing.
    Connecting,    ///< Association in progress.
    Connected,     ///< Associated AND holding an IP address.
    Disconnected,  ///< Was connected or tried to; retrying with backoff.
    AccessPoint,   ///< Running the first-run setup AP.
};

const char* toString(WifiState state);

class WifiManager {
  public:
    /// Called on every state change. Runs on the system event task — see the threading note.
    using StateCallback = std::function<void(WifiState state, bool online)>;

    /// Bring up netif, the event loop and the Wi-Fi driver (and the C6 power rail).
    /// Does not connect. Safe to call once.
    esp_err_t begin();

    /// Associate with an access point. An empty SSID is rejected rather than attempted.
    ///
    /// Returns as soon as the attempt starts; watch the state callback for the outcome.
    /// Reconnection afterwards is automatic, with exponential backoff.
    esp_err_t connect(const char* ssid, const char* password);

    /// Start the first-run configuration access point (open, no password — it exists precisely
    /// because no credentials are known yet, and a password would have to be printed somewhere
    /// anyway). Stops any station activity first.
    esp_err_t startAccessPoint(const char* ssid);

    /// Drop the connection and stop retrying.
    void disconnect();

    WifiState state() const { return state_; }

    /// True only when associated AND holding an IP address. Association alone is not enough to
    /// make an HTTPS request, and treating it as "online" produces confusing early failures.
    bool online() const { return state_ == WifiState::Connected; }

    void setStateCallback(StateCallback callback) { callback_ = std::move(callback); }

    /// Dotted-quad IP address, or "0.0.0.0" when not connected.
    void ipAddress(char* out, size_t capacity) const;

    /// Signal strength in dBm, or 0 when not connected.
    int rssi() const;

    /// Scan for access points and log what is found.
    ///
    /// Primarily a bring-up diagnostic: it exercises the entire host-to-C6 path without needing
    /// credentials, which makes it the cheapest possible proof that the SDIO link works. Also
    /// the source of the network list in the setup portal.
    esp_err_t scanAndLog(size_t max_results = 20);

  private:
    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void onWifiEvent(int32_t id, void* data);
    void onIpEvent(int32_t id, void* data);
    void setState(WifiState state);
    void scheduleReconnect();

    StateCallback callback_;
    WifiState state_ = WifiState::Idle;
    bool started_ = false;

    /// Consecutive failures, used to grow the retry delay. Reset on success.
    int retry_count_ = 0;
};

}  // namespace dashboard::net
