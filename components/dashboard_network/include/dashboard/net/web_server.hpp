// The device's configuration web server.
//
// WHAT IT IS FOR
//
// The Tab5 has a touchscreen and no keyboard. Toggles and brightness are fine to drive by touch,
// but a place name, a POSIX timezone rule or an API token entered through an on-screen keyboard
// is miserable. So configuration that involves typing happens in a browser instead.
//
// It serves two pages from one server, because there is one port 80:
//
//   /          Wi-Fi. On first run this is the setup portal, reached over the device's own
//              access point. Afterwards it is how you move the dashboard to another network.
//   /settings  Everything else: weather location, timezone, clock face.
//
// WHERE IT RUNS
//
// Both, and it does not stop. httpd binds every interface, so the same server answers on the
// setup access point and on your home network once the device joins it. Started when the radio
// comes up and left running: a settings page that only existed during first-run setup would be
// no use for the thing it is mostly needed for.
//
// PLAIN HTTP, DELIBERATELY
//
// HTTPS here would need a self-signed certificate that every browser warns about, which teaches
// people to click through TLS warnings — worse than what this accepts: a LAN hop, on a network
// you control. Recorded as a known limitation in docs/IMPLEMENTATION_PLAN.md.
//
// AUTHENTICATION
//
// Gated by the same PIN as the dashboard's lock screen, stored salted-and-hashed by SecretStore
// and verified per request against an X-Dash-Pin header. There is no session state on the device.
//
// One deliberate hole: when no PIN has been set, everything is open. The device cannot demand a
// credential it has never been given, and refusing would lock the user out of the only interface
// that can set one. The settings page notices and asks for a PIN to be chosen on first visit.
//
// THREADING
//
// Handlers run on the HTTP server's own task. The callbacks below are invoked from there, so
// whatever they touch must tolerate it — which is why writing settings is a callback into the
// owner rather than this component mutating a live Settings object under the UI thread's feet.

#pragma once

#include <functional>

#include "esp_err.h"
#include "esp_http_server.h"

#include "dashboard/storage/settings.hpp"

namespace dashboard::net {

class WifiManager;

class WebServer {
  public:
    /// Persist submitted Wi-Fi credentials. Return the result of the write: it is what the user
    /// is told, so a failed save must not report success.
    ///
    /// Should NOT attempt the connection — see the note in the .cpp about why the server leaves
    /// that to the supervisor.
    using WifiSubmit = std::function<esp_err_t(const char* ssid, const char* password)>;

    /// Fill `out` with a snapshot of the current settings, for rendering.
    using SettingsRead = std::function<void(dashboard::storage::Settings& out)>;

    /// Apply and persist an edited copy. The server never mutates the live Settings itself; it
    /// hands over a complete object and lets the owner install it wherever it belongs.
    using SettingsWrite = std::function<esp_err_t(const dashboard::storage::Settings& incoming)>;

    struct Callbacks {
        WifiSubmit on_wifi;
        SettingsRead read_settings;
        SettingsWrite write_settings;
    };

    /// Start serving. `wifi` supplies the network list and must outlive this object.
    /// Safe to call when already running: a no-op reporting success.
    esp_err_t start(WifiManager& wifi, Callbacks callbacks);

    /// Stop serving and release the socket. Safe to call when not running.
    esp_err_t stop();

    bool running() const { return server_ != nullptr; }

  private:
    static esp_err_t handleSetupPage(httpd_req_t* req);
    static esp_err_t handleSettingsPage(httpd_req_t* req);
    static esp_err_t handleStyle(httpd_req_t* req);
    static esp_err_t handleScript(httpd_req_t* req);

    static esp_err_t handleState(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleWifiPost(httpd_req_t* req);
    static esp_err_t handleSettingsGet(httpd_req_t* req);
    static esp_err_t handleSettingsPost(httpd_req_t* req);
    static esp_err_t handlePinPost(httpd_req_t* req);

    static esp_err_t handleNotFound(httpd_req_t* req, httpd_err_code_t error);

    httpd_handle_t server_ = nullptr;
    WifiManager* wifi_ = nullptr;
    Callbacks callbacks_;
};

}  // namespace dashboard::net
