// The first-run setup portal: an HTTP server on the device's own access point.
//
// WHAT THIS IS FOR
//
// A device with no credentials cannot be configured over the network it is not on, and the Tab5
// has no keyboard. So it raises an access point, serves one page over plain HTTP, and takes the
// SSID and password from a form. WifiManager brings the AP up; this serves what is on it.
//
// PLAIN HTTP, DELIBERATELY
//
// Serving HTTPS here would need a self-signed certificate that every phone warns about, which
// teaches users to click through TLS warnings — a worse outcome than the exposure this accepts:
// one short-lived session, on an access point with one client, carrying one Wi-Fi password.
// Recorded as a known limitation in docs/IMPLEMENTATION_PLAN.md.
//
// THREADING
//
// Handlers run on the HTTP server's own task, NOT the LVGL thread and not the caller's. The
// submit callback is invoked from there, so whatever it touches must tolerate that.
//
// NO CAPTIVE-PORTAL DNS
//
// There is no DNS hijack, so no automatic "sign in to network" popup appears. The user browses
// to the AP's address, which the device shows on its own screen — see WifiManager::apIpAddress().

#pragma once

#include <functional>

#include "esp_err.h"
#include "esp_http_server.h"

namespace dashboard::net {

class WifiManager;

class ProvisioningServer {
  public:
    /// Invoked when the form is submitted, on the HTTP server task.
    ///
    /// Expected to persist the credentials and return the result of doing so: the return value
    /// is what the user is told, so a failed NVS write must not report success. The handler
    /// should NOT attempt the connection itself — see the note in the .cpp about why the portal
    /// deliberately leaves that to the supervisor.
    using SubmitHandler = std::function<esp_err_t(const char* ssid, const char* password)>;

    /// Start serving. `wifi` supplies the network list and must outlive this object.
    /// Safe to call when already running: it is a no-op that reports success.
    esp_err_t start(WifiManager& wifi, SubmitHandler on_submit);

    /// Stop serving and release the socket. Safe to call when not running.
    esp_err_t stop();

    bool running() const { return server_ != nullptr; }

  private:
    static esp_err_t handleRoot(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleSave(httpd_req_t* req);
    static esp_err_t handleNotFound(httpd_req_t* req, httpd_err_code_t error);

    httpd_handle_t server_ = nullptr;
    WifiManager* wifi_ = nullptr;
    SubmitHandler on_submit_;
};

}  // namespace dashboard::net
