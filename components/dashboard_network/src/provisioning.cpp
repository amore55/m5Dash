#include "dashboard/net/provisioning.hpp"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"

#include "dashboard/net/wifi_manager.hpp"

namespace dashboard::net {
namespace {

constexpr const char* kTag = "portal";

/// Networks offered on the page. More than this is a list nobody scrolls, and the array lives on
/// the HTTP task's stack.
constexpr size_t kPortalScanResults = 20;

/// Largest form body accepted. An SSID is at most 32 bytes and a WPA2 passphrase 63, and
/// percent-encoding can triple each; 512 leaves room without letting a client send us anything
/// interesting.
constexpr size_t kMaxFormBytes = 512;

/// 63 characters plus the terminator, per WPA2.
constexpr size_t kMaxPassphrase = 64;

/// memset the optimiser is not permitted to discard. A plain memset on a local buffer that is
/// never read again is dead-store-eliminated by any decent compiler, which is exactly the case
/// here and exactly when it matters.
void secureZero(void* p, size_t n) {
    // Pointer to volatile, not a volatile pointer: it is the writes that must survive the
    // optimiser, and incrementing a volatile-qualified pointer is deprecated besides.
    auto* q = static_cast<volatile unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        q[i] = 0;
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Percent-decode `in_len` bytes of form data into `out`. Always NUL-terminates.
///
/// A malformed escape is passed through literally rather than rejected: a password containing a
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
        if (cursor != nullptr) {
            ++cursor;
        }
    }
    out[0] = '\0';
    return false;
}

/// Escape a scanned SSID for embedding in JSON. SSIDs are arbitrary bytes, not text, so a quote
/// or backslash in one would otherwise produce a response the page cannot parse.
void jsonEscape(const char* in, char* out, size_t capacity) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == '"' || c == '\\') {
            if (o + 3 >= capacity) {
                break;
            }
            out[o++] = '\\';
            out[o++] = static_cast<char>(c);
        } else if (c < 0x20) {
            if (o + 7 >= capacity) {
                break;
            }
            o += static_cast<size_t>(std::snprintf(out + o, capacity - o, "\\u%04X", c));
        } else {
            if (o + 2 >= capacity) {
                break;
            }
            out[o++] = static_cast<char>(c);
        }
    }
    out[o] = '\0';
}

/// The whole portal, in one response.
///
/// Everything is inline because it has to be: the device is an access point with no route to the
/// internet, so a CDN stylesheet or font would simply hang. Colours follow the dashboard theme so
/// the page looks like it belongs to the device that served it.
constexpr const char* kIndexHtml = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk Dashboard setup</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 24px 16px 48px;
    background: #0B0D10; color: #E6E9EF;
    font: 16px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }
  main { max-width: 30rem; margin: 0 auto; }
  h1 { font-size: 1.35rem; margin: 0 0 .25rem; }
  p.sub { margin: 0 0 1.5rem; color: #8A93A6; font-size: .9rem; }
  label { display: block; margin: 1rem 0 .35rem; font-size: .85rem; color: #8A93A6;
          text-transform: uppercase; letter-spacing: .04em; }
  input {
    width: 100%; padding: .7rem .8rem; font-size: 1rem;
    background: #15181E; color: #E6E9EF;
    border: 1px solid #262B35; border-radius: 8px;
  }
  input:focus { outline: 2px solid #4C8DFF; outline-offset: 1px; border-color: #4C8DFF; }
  button {
    width: 100%; margin-top: 1.5rem; padding: .8rem; font-size: 1rem; font-weight: 600;
    background: #4C8DFF; color: #08101F; border: 0; border-radius: 8px; cursor: pointer;
  }
  button:disabled { opacity: .5; cursor: default; }
  #nets { list-style: none; margin: .5rem 0 0; padding: 0;
          border: 1px solid #262B35; border-radius: 8px; overflow: hidden; }
  #nets li { border-top: 1px solid #262B35; }
  #nets li:first-child { border-top: 0; }
  #nets button.net {
    margin: 0; border-radius: 0; background: transparent; color: #E6E9EF; font-weight: 400;
    text-align: left; padding: .65rem .8rem; display: flex; gap: .6rem; align-items: baseline;
  }
  #nets button.net:hover { background: #15181E; }
  #nets .ssid { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  #nets .meta { color: #8A93A6; font-size: .8rem; font-variant-numeric: tabular-nums; }
  .note { color: #8A93A6; font-size: .85rem; margin: .75rem 0 0; }
  #msg { margin-top: 1.25rem; padding: .7rem .8rem; border-radius: 8px; display: none; }
  #msg.ok  { display: block; background: #10261A; color: #7BE0A6; border: 1px solid #1F5136; }
  #msg.err { display: block; background: #2A1416; color: #FF9B9B; border: 1px solid #5A2226; }
</style>
</head>
<body>
<main>
  <h1>Desk Dashboard</h1>
  <p class="sub">Choose your Wi-Fi network to finish setup.</p>

  <p class="note" id="scanning">Scanning&hellip;</p>
  <ul id="nets"></ul>

  <form id="f" autocomplete="off">
    <label for="ssid">Network name</label>
    <input id="ssid" name="ssid" maxlength="32" required autocapitalize="none" spellcheck="false">
    <label for="password">Password</label>
    <input id="password" name="password" type="password" maxlength="63" autocomplete="off">
    <p class="note">Leave the password empty for an open network.</p>
    <button type="submit" id="go">Save and connect</button>
  </form>

  <div id="msg"></div>
</main>
<script>
(function () {
  var msg = document.getElementById('msg');
  var ssid = document.getElementById('ssid');

  function say(text, kind) { msg.textContent = text; msg.className = kind; }

  function bars(rssi) {
    if (rssi >= -55) return '••••';
    if (rssi >= -67) return '•••';
    if (rssi >= -78) return '••';
    return '•';
  }

  fetch('/scan').then(function (r) { return r.json(); }).then(function (data) {
    var note = document.getElementById('scanning');
    var list = document.getElementById('nets');
    var nets = (data && data.networks) || [];
    if (!nets.length) {
      note.textContent = 'No networks found. Enter the name by hand.';
      return;
    }
    note.style.display = 'none';
    nets.forEach(function (n) {
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'net';
      var name = document.createElement('span');
      name.className = 'ssid';
      name.textContent = n.ssid;
      var meta = document.createElement('span');
      meta.className = 'meta';
      meta.textContent = (n.secured ? '\u{1F512} ' : '') + bars(n.rssi);
      b.appendChild(name);
      b.appendChild(meta);
      b.addEventListener('click', function () {
        ssid.value = n.ssid;
        document.getElementById('password').focus();
      });
      var li = document.createElement('li');
      li.appendChild(b);
      list.appendChild(li);
    });
  }).catch(function () {
    document.getElementById('scanning').textContent =
      'Could not scan. Enter the network name by hand.';
  });

  document.getElementById('f').addEventListener('submit', function (e) {
    e.preventDefault();
    var go = document.getElementById('go');
    var body = 'ssid=' + encodeURIComponent(ssid.value) +
               '&password=' + encodeURIComponent(document.getElementById('password').value);
    go.disabled = true;
    say('Saving…', 'ok');
    fetch('/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body
    }).then(function (r) { return r.json(); }).then(function (res) {
      if (res && res.ok) {
        say('Saved. The dashboard is connecting — this network will disappear ' +
            'once it succeeds.', 'ok');
      } else {
        say((res && res.error) || 'Could not save those details.', 'err');
        go.disabled = false;
      }
    }).catch(function () {
      say('Could not reach the dashboard. Check you are still on its network.', 'err');
      go.disabled = false;
    });
  });
})();
</script>
</body>
</html>
)PAGE";

}  // namespace

esp_err_t ProvisioningServer::handleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // The page is regenerated on every boot of the portal and must never be cached: a stale copy
    // would be served against a device whose state has moved on.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, kIndexHtml);
}

esp_err_t ProvisioningServer::handleScan(httpd_req_t* req) {
    auto* self = static_cast<ProvisioningServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (self == nullptr || self->wifi_ == nullptr) {
        return httpd_resp_send_500(req);
    }

    ScanResult results[kPortalScanResults];
    size_t count = 0;
    const esp_err_t err = self->wifi_->scan(results, kPortalScanResults, count);
    if (err != ESP_OK) {
        // An empty list is a truthful answer the page knows how to render — it falls back to
        // asking for the name by hand. A 500 would be a dead end for the same information.
        ESP_LOGW(kTag, "scan for the portal failed: %s", esp_err_to_name(err));
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }

    ESP_LOGI(kTag, "serving %u networks to the setup page", static_cast<unsigned>(count));

    // Chunked, so the response size is bounded by one record rather than by the whole list.
    httpd_resp_send_chunk(req, "{\"networks\":[", HTTPD_RESP_USE_STRLEN);
    char escaped[6 * 32 + 1];
    char item[sizeof(escaped) + 96];
    for (size_t i = 0; i < count; ++i) {
        jsonEscape(results[i].ssid, escaped, sizeof(escaped));
        const int n = std::snprintf(
            item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"secured\":%s}",
            (i == 0) ? "" : ",", escaped, results[i].rssi,
            static_cast<unsigned>(results[i].channel), results[i].secured ? "true" : "false");
        if (n > 0) {
            httpd_resp_send_chunk(req, item, static_cast<size_t>(n));
        }
    }
    httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t ProvisioningServer::handleSave(httpd_req_t* req) {
    auto* self = static_cast<ProvisioningServer*>(req->user_ctx);
    httpd_resp_set_type(req, "application/json");

    if (req->content_len == 0 || req->content_len > kMaxFormBytes) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Malformed request.\"}");
    }

    char body[kMaxFormBytes + 1];
    size_t received = 0;
    while (received < req->content_len) {
        const int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            secureZero(body, sizeof(body));
            return ESP_FAIL;  // the socket is gone; there is nobody to answer
        }
        received += static_cast<size_t>(r);
    }
    body[received] = '\0';

    char ssid[33] = {};
    char password[kMaxPassphrase] = {};
    findFormField(body, "ssid", ssid, sizeof(ssid));
    findFormField(body, "password", password, sizeof(password));
    // The body still holds the passphrase in encoded form. It is no longer needed.
    secureZero(body, sizeof(body));

    if (ssid[0] == '\0') {
        secureZero(password, sizeof(password));
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"A network name is required.\"}");
    }

    // The SSID is safe to log; the passphrase is never passed to a log call, here or anywhere.
    ESP_LOGI(kTag, "credentials submitted for '%s'", ssid);

    const esp_err_t err = self != nullptr && self->on_submit_
                              ? self->on_submit_(ssid, password)
                              : ESP_ERR_INVALID_STATE;
    secureZero(password, sizeof(password));

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "storing credentials failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(
            req, "{\"ok\":false,\"error\":\"The dashboard could not save those details.\"}");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t ProvisioningServer::handleNotFound(httpd_req_t* req, httpd_err_code_t) {
    // Send every unknown path to the form. With no DNS hijack this only rescues someone who
    // typed a stray URL, but it costs nothing and turns a 404 into the page they wanted.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t ProvisioningServer::start(WifiManager& wifi, SubmitHandler on_submit) {
    if (server_ != nullptr) {
        return ESP_OK;
    }
    wifi_ = &wifi;
    on_submit_ = std::move(on_submit);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // The scan handler puts a ScanResult array and the JSON assembly buffers on this stack.
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 4;
    // One phone, one page, and a browser that opens several sockets: recycle the oldest rather
    // than refusing a connection, which on a setup portal reads as the device being broken.
    cfg.lru_purge_enable = true;

    const esp_err_t err = httpd_start(&server_, &cfg);
    if (err != ESP_OK) {
        server_ = nullptr;
        on_submit_ = nullptr;
        ESP_LOGE(kTag, "the setup portal server failed to start: %s", esp_err_to_name(err));
        return err;
    }

    const auto route = [this](const char* uri, httpd_method_t method,
                              esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t entry = {};
        entry.uri = uri;
        entry.method = method;
        entry.handler = handler;
        entry.user_ctx = this;
        return httpd_register_uri_handler(server_, &entry);
    };

    route("/", HTTP_GET, &handleRoot);
    route("/scan", HTTP_GET, &handleScan);
    route("/save", HTTP_POST, &handleSave);
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, &handleNotFound);

    ESP_LOGI(kTag, "setup portal serving on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t ProvisioningServer::stop() {
    if (server_ == nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = httpd_stop(server_);
    server_ = nullptr;
    on_submit_ = nullptr;
    wifi_ = nullptr;
    ESP_LOGI(kTag, "setup portal stopped");
    return err;
}

}  // namespace dashboard::net
