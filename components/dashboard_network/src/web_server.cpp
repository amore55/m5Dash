#include "dashboard/net/web_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dashboard/net/wifi_manager.hpp"
#include "dashboard/storage/secret_store.hpp"
#include "dashboard/time_utils.hpp"

namespace dashboard::net {
namespace {

constexpr const char* kTag = "web";

/// Networks offered on the Wi-Fi page. More is a list nobody scrolls, and the array lives on the
/// HTTP task's stack.
constexpr size_t kPortalScanResults = 20;

/// Largest form body accepted. The biggest realistic one is the settings form, which is a few
/// short fields; 1 kB leaves room without letting a client send us anything interesting.
constexpr size_t kMaxFormBytes = 1024;

/// 63 characters plus the terminator, per WPA2.
constexpr size_t kMaxPassphrase = 64;

/// Pause after a rejected PIN.
///
/// A four-digit PIN over HTTP would otherwise be exhaustible in seconds. Half a second caps an
/// attacker at a couple of guesses per second and is imperceptible to someone who simply
/// mistyped. Deliberately a delay rather than a lockout: locking the settings page is locking the
/// only convenient way to fix the device.
constexpr TickType_t kBadPinDelay = pdMS_TO_TICKS(500);

// Pages and assets, linked in by EMBED_TXTFILES (see CMakeLists.txt), which appends a NUL — so
// these are ordinary C strings and can be sent with httpd_resp_sendstr.
extern "C" const char kSetupHtml[] asm("_binary_setup_html_start");
extern "C" const char kSettingsHtml[] asm("_binary_settings_html_start");
extern "C" const char kStyleCss[] asm("_binary_style_css_start");
extern "C" const char kAppJs[] asm("_binary_app_js_start");

// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

/// memset the optimiser is not permitted to discard. A plain memset on a local that is never read
/// again is exactly what dead-store elimination removes, and exactly when it matters.
void secureZero(void* p, size_t n) {
    auto* q = static_cast<volatile unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        q[i] = 0;
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Percent-decode `in_len` bytes of form data into `out`. Always NUL-terminates.
///
/// A malformed escape is passed through literally rather than rejected: a passphrase containing a
/// stray '%' should still reach the radio, which is the only thing that can judge it.
void urlDecode(const char* in, size_t in_len, char* out, size_t out_capacity) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_capacity; ++i) {
        if (in[i] == '+') {
            out[o++] = ' ';
        } else if (in[i] == '%' && i + 2 < in_len) {
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = static_cast<char>(hi * 16 + lo);
                i += 2;
            } else {
                out[o++] = in[i];
            }
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

/// Extract one field from an application/x-www-form-urlencoded body.
///
/// Only matches at a field boundary, so a field named "password" is not found inside
/// "wifi_password" — a subtlety a plain strstr would get wrong.
bool findFormField(const char* body, const char* name, char* out, size_t out_capacity) {
    const size_t name_len = std::strlen(name);
    const char* cursor = body;
    while (cursor != nullptr && *cursor != '\0') {
        if (std::strncmp(cursor, name, name_len) == 0 && cursor[name_len] == '=') {
            const char* value = cursor + name_len + 1;
            const char* end = std::strchr(value, '&');
            const size_t len =
                (end != nullptr) ? static_cast<size_t>(end - value) : std::strlen(value);
            urlDecode(value, len, out, out_capacity);
            return true;
        }
        cursor = std::strchr(cursor, '&');
        if (cursor != nullptr) ++cursor;
    }
    out[0] = '\0';
    return false;
}

/// Escape a string for embedding in JSON. Scanned SSIDs are arbitrary bytes rather than text, and
/// a settings field is whatever the user typed; either could otherwise break the response.
void jsonEscape(const char* in, char* out, size_t capacity) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '"' || c == '\\') {
            if (o + 3 >= capacity) break;
            out[o++] = '\\';
            out[o++] = static_cast<char>(c);
        } else if (c < 0x20) {
            if (o + 7 >= capacity) break;
            o += static_cast<size_t>(std::snprintf(out + o, capacity - o, "\\u%04X", c));
        } else {
            if (o + 2 >= capacity) break;
            out[o++] = static_cast<char>(c);
        }
    }
    out[o] = '\0';
}

esp_err_t sendJsonError(httpd_req_t* req, const char* status, const char* message) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status);
    char escaped[192];
    jsonEscape(message, escaped, sizeof(escaped));
    char body[256];
    std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    return httpd_resp_sendstr(req, body);
}

/// Serve one embedded asset.
esp_err_t sendAsset(httpd_req_t* req, const char* body, const char* type) {
    httpd_resp_set_type(req, type);
    // Never cached: a stale copy would be served against a device whose state has moved on, and
    // the whole point of these pages is to reflect what the device currently thinks.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/// Is this request allowed to read or change configuration?
///
/// Open when no PIN has been set — see the header for why that hole is deliberate. Otherwise the
/// X-Dash-Pin header must verify against the stored salted hash.
bool authorised(httpd_req_t* req) {
    if (!dashboard::storage::SecretStore::hasLockPin()) {
        return true;
    }

    char pin[dashboard::storage::SecretStore::kMaxPinLength + 1] = {};
    const esp_err_t err = httpd_req_get_hdr_value_str(req, "X-Dash-Pin", pin, sizeof(pin));
    if (err != ESP_OK) {
        return false;  // absent or longer than any valid PIN
    }

    const bool ok = dashboard::storage::SecretStore::verifyLockPin(pin);
    secureZero(pin, sizeof(pin));
    if (!ok) {
        ESP_LOGW(kTag, "rejected a request with a bad PIN");
        vTaskDelay(kBadPinDelay);
    }
    return ok;
}

/// Read a bounded form body. Returns false having already answered the request on failure.
bool readForm(httpd_req_t* req, char* body, size_t capacity) {
    if (req->content_len == 0 || req->content_len >= capacity) {
        sendJsonError(req, "400 Bad Request", "Malformed request.");
        return false;
    }
    size_t received = 0;
    while (received < req->content_len) {
        const int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            secureZero(body, capacity);
            return false;  // socket gone; nobody to answer
        }
        received += static_cast<size_t>(r);
    }
    body[received] = '\0';
    return true;
}

/// Parse a double, keeping `fallback` if the field is absent or not a number.
double formDouble(const char* body, const char* name, double fallback) {
    char raw[48];
    if (!findFormField(body, name, raw, sizeof(raw)) || raw[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(raw, &end);
    // Reject trailing rubbish rather than silently accepting "51.5abc" as 51.5 — a coordinate
    // that is quietly wrong is worse than one that is refused.
    if (end == raw || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return value;
}

/// Parse a whole number, keeping `fallback` if the field is absent or malformed.
///
/// Same strictness as formDouble: trailing rubbish is a rejection, not something to salvage. A
/// brightness that is quietly wrong is a screen you cannot read.
int32_t formInt(const char* body, const char* name, int32_t fallback) {
    char raw[24];
    if (!findFormField(body, name, raw, sizeof(raw)) || raw[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(raw, &end, 10);
    if (end == raw || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return static_cast<int32_t>(value);
}

/// Parse an "HH:MM" field into minutes since midnight, keeping `fallback` if absent or malformed.
/// The browser's <input type="time"> submits exactly this shape.
int32_t formHhMm(const char* body, const char* name, int32_t fallback) {
    char raw[16];
    if (!findFormField(body, name, raw, sizeof(raw)) || raw[0] == '\0') {
        return fallback;
    }
    int minutes = 0;
    if (!dashboard::timeutil::parseHhMm(raw, minutes)) {
        return fallback;
    }
    return static_cast<int32_t>(minutes);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// Static assets
// ---------------------------------------------------------------------------------------

esp_err_t WebServer::handleSetupPage(httpd_req_t* req) {
    return sendAsset(req, kSetupHtml, "text/html; charset=utf-8");
}

esp_err_t WebServer::handleSettingsPage(httpd_req_t* req) {
    return sendAsset(req, kSettingsHtml, "text/html; charset=utf-8");
}

esp_err_t WebServer::handleStyle(httpd_req_t* req) {
    return sendAsset(req, kStyleCss, "text/css; charset=utf-8");
}

esp_err_t WebServer::handleScript(httpd_req_t* req) {
    return sendAsset(req, kAppJs, "application/javascript; charset=utf-8");
}

// ---------------------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------------------

esp_err_t WebServer::handleState(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // Deliberately UNAUTHENTICATED, and deliberately dull. The pages need to know whether to ask
    // for a PIN or to offer setting one before they can ask for anything, so this cannot itself
    // require a PIN. It therefore carries nothing sensitive: no secrets, no coordinates, just
    // enough for the page to decide which form to draw.
    dashboard::storage::Settings snapshot;
    if (self != nullptr && self->callbacks_.read_settings) {
        self->callbacks_.read_settings(snapshot);
    }

    char ssid[3 * 64];
    jsonEscape(snapshot.wifi_ssid.c_str(), ssid, sizeof(ssid));

    char body[320];
    std::snprintf(body, sizeof(body),
                  "{\"ok\":true,\"provisioned\":%s,\"has_pin\":%s,\"ssid\":\"%s\"}",
                  snapshot.provisioned() ? "true" : "false",
                  dashboard::storage::SecretStore::hasLockPin() ? "true" : "false", ssid);
    return httpd_resp_sendstr(req, body);
}

esp_err_t WebServer::handleScan(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!authorised(req)) {
        return sendJsonError(req, "401 Unauthorized", "PIN required.");
    }
    if (self == nullptr || self->wifi_ == nullptr) {
        return httpd_resp_send_500(req);
    }

    ScanResult results[kPortalScanResults];
    size_t count = 0;
    if (self->wifi_->scan(results, kPortalScanResults, count) != ESP_OK) {
        // An empty list is a truthful answer the page knows how to render — it falls back to
        // asking for the name by hand. A 500 would be a dead end carrying the same information.
        ESP_LOGW(kTag, "scan for the setup page failed");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"networks\":[]}");
    }

    httpd_resp_send_chunk(req, "{\"ok\":true,\"networks\":[", HTTPD_RESP_USE_STRLEN);
    char escaped[6 * 32 + 1];
    char item[sizeof(escaped) + 96];
    for (size_t i = 0; i < count; ++i) {
        jsonEscape(results[i].ssid, escaped, sizeof(escaped));
        const int n = std::snprintf(
            item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"secured\":%s}",
            (i == 0) ? "" : ",", escaped, results[i].rssi,
            static_cast<unsigned>(results[i].channel), results[i].secured ? "true" : "false");
        if (n > 0) httpd_resp_send_chunk(req, item, static_cast<size_t>(n));
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t WebServer::handleWifiPost(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");

    if (!authorised(req)) {
        return sendJsonError(req, "401 Unauthorized", "PIN required.");
    }

    char body[kMaxFormBytes + 1];
    if (!readForm(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    char ssid[33] = {};
    char password[kMaxPassphrase] = {};
    findFormField(body, "ssid", ssid, sizeof(ssid));
    findFormField(body, "password", password, sizeof(password));
    // The body still holds the passphrase in encoded form. It is no longer needed.
    secureZero(body, sizeof(body));

    if (ssid[0] == '\0') {
        secureZero(password, sizeof(password));
        return sendJsonError(req, "400 Bad Request", "A network name is required.");
    }

    // The SSID is safe to log; the passphrase is never passed to a log call, here or anywhere.
    ESP_LOGI(kTag, "credentials submitted for '%s'", ssid);

    const esp_err_t err = (self != nullptr && self->callbacks_.on_wifi)
                              ? self->callbacks_.on_wifi(ssid, password)
                              : ESP_ERR_INVALID_STATE;
    secureZero(password, sizeof(password));

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "storing credentials failed: %s", esp_err_to_name(err));
        return sendJsonError(req, "500 Internal Server Error",
                             "The dashboard could not save those details.");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t WebServer::handleSettingsGet(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!authorised(req)) {
        return sendJsonError(req, "401 Unauthorized", "PIN required.");
    }
    if (self == nullptr || !self->callbacks_.read_settings) {
        return httpd_resp_send_500(req);
    }

    dashboard::storage::Settings s;
    self->callbacks_.read_settings(s);

    // No secrets here, and none by accident either: secrets live in SecretStore and are not
    // members of Settings, so there is nothing in this object that could leak.
    char label[3 * 64];
    char tz[3 * 128];
    char face[3 * 32];
    jsonEscape(s.weather_label.c_str(), label, sizeof(label));
    jsonEscape(s.timezone.c_str(), tz, sizeof(tz));
    jsonEscape(s.clock_style.c_str(), face, sizeof(face));

    // Dim window as "HH:MM", which is what <input type="time"> both expects and submits.
    char dim_start[8];
    char dim_end[8];
    dashboard::timeutil::formatHhMm(dim_start, sizeof(dim_start), s.dim_start_minutes);
    dashboard::timeutil::formatHhMm(dim_end, sizeof(dim_end), s.dim_end_minutes);

    // Sized for the escape buffers above at their worst case rather than their realistic one:
    // -Werror=format-truncation reasons about every %s being full and every %f being a 300-digit
    // double, not about a place name and a latitude.
    char github_user[3 * 32];
    jsonEscape(s.github_username.c_str(), github_user, sizeof(github_user));

    // PRESENCE, never content. has_* is what lets the page render "a token is stored — type a
    // new one to replace it" without the token ever leaving the device.
    const bool has_github_token =
        dashboard::storage::SecretStore::has(dashboard::storage::Secret::GithubToken);
    const bool has_quote_key =
        dashboard::storage::SecretStore::has(dashboard::storage::Secret::QuoteApiKey);

    char body[2304];
    std::snprintf(body, sizeof(body),
                  "{\"ok\":true,\"weather_label\":\"%s\",\"latitude\":%.6f,\"longitude\":%.6f,"
                  "\"timezone\":\"%s\",\"clock_style\":\"%s\",\"show_seconds\":%s,"
                  "\"brightness\":%ld,\"night_brightness\":%ld,"
                  "\"dim_start\":\"%s\",\"dim_end\":\"%s\",\"min_brightness\":%ld,"
                  "\"github_username\":\"%s\",\"has_github_token\":%s,"
                  "\"has_quote_api_key\":%s}",
                  label, s.latitude, s.longitude, tz, face, s.show_seconds ? "true" : "false",
                  static_cast<long>(s.brightness_percent),
                  static_cast<long>(s.night_brightness_percent), dim_start, dim_end,
                  static_cast<long>(dashboard::storage::kMinDayBrightnessPercent), github_user,
                  has_github_token ? "true" : "false", has_quote_key ? "true" : "false");
    return httpd_resp_sendstr(req, body);
}

esp_err_t WebServer::handleSettingsPost(httpd_req_t* req) {
    auto* self = static_cast<WebServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");

    if (!authorised(req)) {
        return sendJsonError(req, "401 Unauthorized", "PIN required.");
    }
    if (self == nullptr || !self->callbacks_.read_settings || !self->callbacks_.write_settings) {
        return httpd_resp_send_500(req);
    }

    char body[kMaxFormBytes + 1];
    if (!readForm(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    // Seeded from the current settings, so a form that carries only some fields changes only
    // those. This page does not know about OTA channels or Telegram, and must not erase them.
    dashboard::storage::Settings edited;
    self->callbacks_.read_settings(edited);

    char text[192];
    if (findFormField(body, "weather_label", text, sizeof(text)) && text[0] != '\0') {
        edited.weather_label.assign(text);
    }
    if (findFormField(body, "timezone", text, sizeof(text)) && text[0] != '\0') {
        edited.timezone.assign(text);
    }
    if (findFormField(body, "clock_style", text, sizeof(text)) && text[0] != '\0') {
        edited.clock_style.assign(text);
    }
    if (findFormField(body, "show_seconds", text, sizeof(text)) && text[0] != '\0') {
        edited.show_seconds = (text[0] == '1' || text[0] == 't');
    }
    edited.latitude = formDouble(body, "latitude", edited.latitude);
    edited.longitude = formDouble(body, "longitude", edited.longitude);

    edited.brightness_percent = formInt(body, "brightness", edited.brightness_percent);
    edited.night_brightness_percent =
        formInt(body, "night_brightness", edited.night_brightness_percent);
    edited.dim_start_minutes = formHhMm(body, "dim_start", edited.dim_start_minutes);
    edited.dim_end_minutes = formHhMm(body, "dim_end", edited.dim_end_minutes);

    if (findFormField(body, "github_username", text, sizeof(text)) && text[0] != '\0') {
        edited.github_username.assign(text);
    }

    // SECRETS ARE WRITE-ONLY FROM HERE. They go straight to SecretStore and are never read back
    // into `edited`, never returned by the GET handler and never logged — the page shows only
    // whether one is present. An ABSENT field leaves the stored secret alone, so saving the form
    // without retyping a token does not wipe it; an explicitly EMPTY field clears it, which is
    // the only way to remove one without a factory reset.
    bool secrets_changed = false;
    char secret[dashboard::storage::kMaxSecretLength + 1] = {};
    if (findFormField(body, "github_token", secret, sizeof(secret))) {
        const esp_err_t stored =
            dashboard::storage::SecretStore::set(dashboard::storage::Secret::GithubToken, secret);
        ESP_LOGI(kTag, "GitHub token %s from the web page",
                 secret[0] == '\0' ? "cleared" : (stored == ESP_OK ? "stored" : "REJECTED"));
        secrets_changed = secrets_changed || stored == ESP_OK;
    }
    secureZero(secret, sizeof(secret));
    if (findFormField(body, "quote_api_key", secret, sizeof(secret))) {
        const esp_err_t stored =
            dashboard::storage::SecretStore::set(dashboard::storage::Secret::QuoteApiKey, secret);
        ESP_LOGI(kTag, "quote API key %s from the web page",
                 secret[0] == '\0' ? "cleared" : (stored == ESP_OK ? "stored" : "REJECTED"));
        secrets_changed = secrets_changed || stored == ESP_OK;
    }
    secureZero(secret, sizeof(secret));

    secureZero(body, sizeof(body));

    // Belt and braces against a hand-crafted POST: the same clamp the loader applies, so the UI
    // cannot be driven into an impossible state from here either.
    edited.clampToValidRanges();

    if (edited.latitude < -90.0 || edited.latitude > 90.0 || edited.longitude < -180.0 ||
        edited.longitude > 180.0) {
        return sendJsonError(req, "400 Bad Request", "Those coordinates are out of range.");
    }

    const esp_err_t err = self->callbacks_.write_settings(edited);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "saving settings failed: %s", esp_err_to_name(err));
        return sendJsonError(req, "500 Internal Server Error",
                             "The dashboard could not save those settings.");
    }
    ESP_LOGI(kTag, "settings updated from the web page");

    // AFTER the settings write, so a plugin refetching on a new credential also sees any changed
    // configuration that arrived in the same POST.
    if (secrets_changed && self->callbacks_.on_secrets_changed) {
        self->callbacks_.on_secrets_changed();
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t WebServer::handlePinPost(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");

    // Covers both cases with one rule: with no PIN set this is open, so a first-run visit can
    // choose one; once set, changing it needs the current one.
    if (!authorised(req)) {
        return sendJsonError(req, "401 Unauthorized", "PIN required.");
    }

    char body[kMaxFormBytes + 1];
    if (!readForm(req, body, sizeof(body))) {
        return ESP_FAIL;
    }

    char pin[dashboard::storage::SecretStore::kMaxPinLength + 1] = {};
    findFormField(body, "pin", pin, sizeof(pin));
    secureZero(body, sizeof(body));

    // An empty PIN removes protection rather than being rejected. Getting here already required
    // the current PIN, and without a way out a forgotten one would mean reflashing the device.
    const size_t len = std::strlen(pin);
    if (len == 0) {
        const esp_err_t cleared = dashboard::storage::SecretStore::clearLockPin();
        if (cleared != ESP_OK) {
            return sendJsonError(req, "500 Internal Server Error",
                                 "The PIN could not be removed.");
        }
        ESP_LOGW(kTag, "lock PIN removed from the web page; settings are now unprotected");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    if (len < dashboard::storage::SecretStore::kMinPinLength ||
        len > dashboard::storage::SecretStore::kMaxPinLength) {
        secureZero(pin, sizeof(pin));
        return sendJsonError(req, "400 Bad Request", "The PIN must be 4 to 8 digits.");
    }

    const esp_err_t err = dashboard::storage::SecretStore::setLockPin(pin);
    secureZero(pin, sizeof(pin));

    if (err != ESP_OK) {
        return sendJsonError(req, "500 Internal Server Error", "The PIN could not be saved.");
    }
    ESP_LOGI(kTag, "lock PIN set from the web page");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t WebServer::handleNotFound(httpd_req_t* req, httpd_err_code_t) {
    // Send unknown paths to the Wi-Fi page. With no DNS hijack this only rescues someone who
    // typed a stray URL, but it costs nothing and turns a 404 into the page they wanted.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------

esp_err_t WebServer::start(WifiManager& wifi, Callbacks callbacks) {
    if (server_ != nullptr) {
        return ESP_OK;
    }
    wifi_ = &wifi;
    callbacks_ = std::move(callbacks);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // The scan handler puts a ScanResult array and the JSON assembly buffers on this stack.
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    // A browser opens several sockets for one page. Recycle the oldest rather than refusing a
    // connection, which on a setup page reads as the device being broken.
    cfg.lru_purge_enable = true;

    const esp_err_t err = httpd_start(&server_, &cfg);
    if (err != ESP_OK) {
        server_ = nullptr;
        wifi_ = nullptr;
        callbacks_ = {};
        ESP_LOGE(kTag, "the configuration server failed to start: %s", esp_err_to_name(err));
        return err;
    }

    const auto route = [this](const char* uri, httpd_method_t method,
                              esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t entry = {};
        entry.uri = uri;
        entry.method = method;
        entry.handler = handler;
        entry.user_ctx = this;
        httpd_register_uri_handler(server_, &entry);
    };

    route("/", HTTP_GET, &handleSetupPage);
    route("/settings", HTTP_GET, &handleSettingsPage);
    route("/style.css", HTTP_GET, &handleStyle);
    route("/app.js", HTTP_GET, &handleScript);

    route("/api/state", HTTP_GET, &handleState);
    route("/api/scan", HTTP_GET, &handleScan);
    route("/api/wifi", HTTP_POST, &handleWifiPost);
    route("/api/settings", HTTP_GET, &handleSettingsGet);
    route("/api/settings", HTTP_POST, &handleSettingsPost);
    route("/api/pin", HTTP_POST, &handlePinPost);

    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, &handleNotFound);

    ESP_LOGI(kTag, "configuration server listening on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t WebServer::stop() {
    if (server_ == nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = httpd_stop(server_);
    server_ = nullptr;
    wifi_ = nullptr;
    callbacks_ = {};
    ESP_LOGI(kTag, "configuration server stopped");
    return err;
}

}  // namespace dashboard::net
