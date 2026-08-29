#include "dashboard/net/https_client.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "version.hpp"

namespace dashboard::net {

// Declared in https_client.hpp, not file-local, because streamGet()'s OTA caller needs to hold
// it across a whole multi-megabyte download rather than one attempt — see the header.
//
/// Each plugin has its own worker task, so without this their handshakes overlap — and a
/// handshake is by far the largest transient demand on internal SRAM this firmware makes.
/// Measured with the weather and Elizabeth line workers colliding on the same millisecond, the
/// internal low-water mark fell from ~40 KB to **14 KB**. That is close enough to the failure
/// that produced a reboot loop (see docs/BACKLOG.md §1.3) to be worth designing out rather than
/// monitoring.
///
/// Serialising makes the peak the cost of ONE handshake no matter how many plugins exist, which
/// means adding the sixth integration cannot quietly reintroduce the crash. The cost is that a
/// plugin may wait for another's request; with refresh intervals of 2 to 20 minutes against a
/// 12 s timeout, that is a rare few seconds on a background task nobody is watching.
///
/// Held per ATTEMPT by get()'s retry loop, not across the whole retry schedule: sleeping out a
/// 14 s backoff while holding it would block every other plugin for the duration, which is a
/// different bug. streamGet() is the one caller that deliberately holds it for the whole call —
/// see the OTA note in the header.
std::mutex& tlsGate() {
    static std::mutex gate;
    return gate;
}

namespace {

constexpr const char* kTag = "https";

/// Longest redacted URL kept for logging. Long enough for a real API path, short enough to sit on
/// a stack alongside everything else a plugin worker is doing.
constexpr size_t kLogUrlBytes = 160;

/// Identifies the firmware to upstreams, which is the polite thing to do and makes our traffic
/// recognisable in someone else's rate-limit logs.
const char* userAgent() {
    static char ua[64] = {};
    if (ua[0] == '\0') {
        std::snprintf(ua, sizeof(ua), "DeskDashboard/%s", dash::kAppVersion);
    }
    return ua;
}

/// Should this failure be tried again?
///
/// Retrying a 404 or a 401 is pointless — the answer will not change, and on an authenticated
/// endpoint repeated failures are how an account gets rate-limited or locked. 5xx and 429 are the
/// server saying "not now", which is exactly what a retry is for.
bool worthRetrying(esp_err_t err, int status) {
    if (err == ESP_ERR_INVALID_SIZE) {
        // The response did not fit. It will not fit next time either, and asking again just makes
        // the caller wait out the whole backoff schedule to be told the same thing.
        return false;
    }
    if (err != ESP_OK) {
        return true;  // transport failure: DNS, TLS, timeout, reset
    }
    return status == 429 || (status >= 500 && status <= 599);
}

/// Exponential backoff, matching the Wi-Fi manager's shape: quick first, then patient.
uint32_t backoffMs(int attempt) {
    uint32_t delay = static_cast<uint32_t>(dash::cfg::kHttpBackoffBaseMs);
    for (int i = 1; i < attempt; ++i) {
        delay *= 2;
    }
    return delay;
}

/// One attempt. Everything that can go wrong is reported; nothing is retried at this level.
esp_err_t attemptGet(const HttpRequest& request, const char* safe_url, char* out, size_t capacity,
                     HttpResponse& response) {
    esp_http_client_config_t cfg = {};
    cfg.url = request.url;
    cfg.timeout_ms = request.timeout_ms;
    cfg.user_agent = userAgent();
    // The bundled root store. There is no code path that disables this.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    // A CREDENTIALLED REQUEST DOES NOT FOLLOW REDIRECTS. This is the one place that decision can
    // be made, so it is made here rather than trusted to each caller.
    //
    // esp_http_client's automatic redirect re-sends the headers it was given to whatever host the
    // Location points at. Those headers include the Authorization we set below. So a compromised,
    // hijacked or merely misconfigured upstream could answer "302 -> https://attacker/" and be
    // handed a live GitHub token or API key — a credential leak requiring no TLS break, because
    // the second leg is a perfectly valid TLS connection to the attacker.
    //
    // Following only SAME-HOST redirects would be the general fix and needs a manual redirect
    // loop. It is not worth building for these APIs: api.github.com and api-ninjas.com answer
    // their documented endpoints directly, so a 3xx on a credentialled request is a sign
    // something is wrong rather than a case to handle. It is surfaced as a failure below.
    //
    // Unauthenticated requests keep following redirects, because nothing is at stake and some
    // upstreams do legitimately redirect.
    const bool carries_credential =
        (request.bearer != nullptr && request.bearer[0] != '\0') ||
        (request.header_value != nullptr && request.header_value[0] != '\0');
    cfg.disable_auto_redirect = carries_credential;
    cfg.max_redirection_count = carries_credential ? 0 : 3;

    const bool is_post = (request.post_body != nullptr);
    cfg.method = is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET;

    // Room for the request line and headers. esp_http_client defaults to 512 bytes, which is not
    // enough for the kind of URL these APIs use: Open-Meteo's forecast query names every variable
    // it should return and comes to about 500 characters on its own, so the request line alone
    // filled the buffer and the client logged
    //     E HTTP_HEADER: Buffer length is small to fit all the headers
    // on every single fetch. TfL and Anthropic take query parameters too, so this is raised here,
    // once, rather than left for each plugin to trip over.
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Header VALUES are set here and never logged, here or anywhere downstream.
    char auth[512];
    if (request.bearer != nullptr && request.bearer[0] != '\0') {
        std::snprintf(auth, sizeof(auth), "Bearer %s", request.bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (request.header_name != nullptr && request.header_value != nullptr) {
        esp_http_client_set_header(client, request.header_name, request.header_value);
    }

    // Opened with the body length up front: esp_http_client's manual (open/write/read) mode needs
    // to know how much it will be asked to write, the same way it needs to know nothing extra for
    // a bodyless GET.
    const int post_len = is_post ? static_cast<int>(std::strlen(request.post_body)) : 0;
    esp_err_t err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "%s: connection failed: %s", safe_url, esp_err_to_name(err));
        // Wipe the Authorization header we built before the stack frame goes away.
        std::memset(auth, 0, sizeof(auth));
        esp_http_client_cleanup(client);
        return err;
    }
    std::memset(auth, 0, sizeof(auth));

    if (is_post) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        const int written = esp_http_client_write(client, request.post_body, post_len);
        if (written != post_len) {
            ESP_LOGW(kTag, "%s: POST body did not write in full", safe_url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    // 0 for a chunked response, whose length is genuinely unknown until it ends. The ceiling is
    // still enforced below, by the read loop.
    const int64_t content_length = esp_http_client_fetch_headers(client);
    response.status = esp_http_client_get_status_code(client);

    // With redirects disabled a 3xx arrives here as the final response, and its body is an
    // error page rather than the JSON the caller asked for. Say what happened instead of
    // handing that body upwards to fail a parse for no visible reason. The Location value is
    // NOT logged: it is attacker-controlled in exactly the scenario this guards against.
    if (carries_credential && response.status >= 300 && response.status < 400) {
        ESP_LOGW(kTag, "%s: refused to follow a %d redirect on a credentialled request",
                 safe_url, response.status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Refuse an oversized body before reading a single byte of it, when the server was honest
    // enough to declare the length. Chunked responses report -1 and are caught by the read loop.
    if (content_length > static_cast<int64_t>(capacity - 1)) {
        ESP_LOGW(kTag, "%s: response is %lld bytes, buffer holds %u", safe_url,
                 static_cast<long long>(content_length), static_cast<unsigned>(capacity - 1));
        response.truncated = true;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = 0;
    while (total + 1 < capacity) {
        const int read = esp_http_client_read(client, out + total, capacity - 1 - total);
        if (read < 0) {
            ESP_LOGW(kTag, "%s: read failed after %u bytes", safe_url,
                     static_cast<unsigned>(total));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read == 0) {
            break;  // end of body
        }
        total += static_cast<size_t>(read);
    }
    out[total] = '\0';
    response.length = total;

    // Did we stop because the body ended, or because the buffer filled?
    //
    // NOT esp_http_client_is_complete_data_received() alone: that reports whether the TRANSPORT
    // finished receiving, not whether the caller consumed what it received. A small chunked body
    // lands whole in the client's internal buffer, so the parser marks the message complete on
    // the first read and that call returns true even with most of the body still undrained —
    // which is exactly how a 326-byte response silently passed for a 64-byte buffer.
    //
    // So ask the question directly: if the buffer is full, try for one more byte.
    if (total + 1 == capacity) {
        char probe = 0;
        if (esp_http_client_read(client, &probe, 1) > 0) {
            response.truncated = true;
        }
    }
    // Still worth keeping: this catches the other shape of incompleteness, a connection that
    // dropped part-way through a body we had room for.
    if (!esp_http_client_is_complete_data_received(client)) {
        response.truncated = true;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (response.truncated) {
        ESP_LOGW(kTag, "%s: response truncated at %u bytes", safe_url,
                 static_cast<unsigned>(total));
        return ESP_ERR_INVALID_SIZE;
    }
    if (response.status < 200 || response.status > 299) {
        ESP_LOGW(kTag, "%s: HTTP %d", safe_url, response.status);
        return ESP_FAIL;
    }

    ESP_LOGD(kTag, "%s: HTTP %d, %u bytes", safe_url, response.status,
             static_cast<unsigned>(total));
    return ESP_OK;
}

/// Read buffer for streamGet(). Independent of any OTA-specific constant on purpose — this file
/// has no reason to know that dash::cfg::kOtaChunkBytes happens to be the same number.
constexpr size_t kStreamChunkBytes = 4096;

/// Shares attemptGet()'s connection setup — credential/redirect policy, headers — but the read
/// loop is fundamentally different (no destination buffer or size ceiling to fill, a sink call
/// per chunk instead), so it is its own function rather than a shared one branching on a
/// "streaming or not" flag partway through.
esp_err_t attemptStreamGet(const HttpRequest& request, const char* safe_url,
                          const HttpsClient::StreamSink& sink, HttpResponse& response) {
    esp_http_client_config_t cfg = {};
    cfg.url = request.url;
    cfg.timeout_ms = request.timeout_ms;
    cfg.user_agent = userAgent();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    // Same redirect policy as attemptGet() — see the long comment there. OTA's manifest and
    // image both currently carry no credential, so this branch is dormant for them today, but
    // the rule is the same regardless of caller.
    const bool carries_credential =
        (request.bearer != nullptr && request.bearer[0] != '\0') ||
        (request.header_value != nullptr && request.header_value[0] != '\0');
    cfg.disable_auto_redirect = carries_credential;
    cfg.max_redirection_count = carries_credential ? 0 : 3;

    cfg.method = HTTP_METHOD_GET;  // streamGet() has no POST caller; add one if that ever changes
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    char auth[512];
    if (request.bearer != nullptr && request.bearer[0] != '\0') {
        std::snprintf(auth, sizeof(auth), "Bearer %s", request.bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (request.header_name != nullptr && request.header_value != nullptr) {
        esp_http_client_set_header(client, request.header_name, request.header_value);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "%s: connection failed: %s", safe_url, esp_err_to_name(err));
        std::memset(auth, 0, sizeof(auth));
        esp_http_client_cleanup(client);
        return err;
    }
    std::memset(auth, 0, sizeof(auth));

    esp_http_client_fetch_headers(client);
    response.status = esp_http_client_get_status_code(client);

    if (carries_credential && response.status >= 300 && response.status < 400) {
        ESP_LOGW(kTag, "%s: refused to follow a %d redirect on a credentialled request",
                 safe_url, response.status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (response.status < 200 || response.status > 299) {
        ESP_LOGW(kTag, "%s: HTTP %d", safe_url, response.status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t chunk[kStreamChunkBytes];
    size_t total = 0;
    for (;;) {
        const int read = esp_http_client_read(client, reinterpret_cast<char*>(chunk),
                                              sizeof(chunk));
        if (read < 0) {
            ESP_LOGW(kTag, "%s: read failed after %u bytes", safe_url,
                     static_cast<unsigned>(total));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (read == 0) {
            break;
        }
        if (!sink(chunk, static_cast<size_t>(read))) {
            // The sink already knows why — a size mismatch against the manifest, a flash write
            // failure — and has logged it. Nothing more to say here.
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        total += static_cast<size_t>(read);
    }
    response.length = total;

    if (!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGW(kTag, "%s: connection dropped after %u bytes", safe_url,
                 static_cast<unsigned>(total));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGD(kTag, "%s: HTTP %d, %u bytes streamed", safe_url, response.status,
             static_cast<unsigned>(total));
    return ESP_OK;
}

}  // namespace

void redactUrl(const char* url, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (url == nullptr) {
        std::snprintf(out, capacity, "(no url)");
        return;
    }
    const char* query = std::strchr(url, '?');
    const size_t keep = (query != nullptr) ? static_cast<size_t>(query - url) : std::strlen(url);
    const size_t copy = (keep < capacity - 1) ? keep : capacity - 1;
    std::memcpy(out, url, copy);
    out[copy] = '\0';
}

void redactUrlHostOnly(const char* url, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (url == nullptr) {
        std::snprintf(out, capacity, "(no url)");
        return;
    }
    const char* scheme_end = std::strstr(url, "://");
    const char* host_start = (scheme_end != nullptr) ? scheme_end + 3 : url;
    const char* path_start = std::strchr(host_start, '/');
    const size_t keep = (path_start != nullptr) ? static_cast<size_t>(path_start - url)
                                                : std::strlen(url);
    const size_t copy = (keep < capacity - 1) ? keep : capacity - 1;
    std::memcpy(out, url, copy);
    out[copy] = '\0';
}

esp_err_t HttpsClient::get(const HttpRequest& request, char* out, size_t capacity,
                           HttpResponse& response) {
    response = HttpResponse{};

    if (out == nullptr || capacity < 2 || request.url == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Plain HTTP is refused rather than downgraded to. Everything this device fetches is either a
    // credentialled API or a firmware image, and neither belongs on an unauthenticated transport.
    if (std::strncmp(request.url, "https://", 8) != 0) {
        ESP_LOGE(kTag, "refusing a non-HTTPS URL");
        return ESP_ERR_INVALID_ARG;
    }
    if (capacity > dash::cfg::kHttpMaxResponseBytes) {
        capacity = dash::cfg::kHttpMaxResponseBytes;
    }

    char safe_url[kLogUrlBytes];
    if (request.path_is_sensitive) {
        redactUrlHostOnly(request.url, safe_url, sizeof(safe_url));
    } else {
        redactUrl(request.url, safe_url, sizeof(safe_url));
    }

    const int attempts = (request.max_attempts > 0) ? request.max_attempts : 1;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        response = HttpResponse{};
        {
            std::lock_guard<std::mutex> gate(tlsGate());
            err = attemptGet(request, safe_url, out, capacity, response);
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (!worthRetrying(err, response.status)) {
            return err;
        }
        if (attempt < attempts) {
            const uint32_t delay = backoffMs(attempt);
            ESP_LOGW(kTag, "%s: attempt %d/%d failed; retrying in %u ms", safe_url, attempt,
                     attempts, static_cast<unsigned>(delay));
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
    }
    return err;
}

esp_err_t HttpsClient::streamGet(const HttpRequest& request, const StreamSink& sink,
                                 HttpResponse& response) {
    response = HttpResponse{};

    if (request.url == nullptr || !sink) {
        return ESP_ERR_INVALID_ARG;
    }
    if (std::strncmp(request.url, "https://", 8) != 0) {
        ESP_LOGE(kTag, "refusing a non-HTTPS URL");
        return ESP_ERR_INVALID_ARG;
    }

    char safe_url[kLogUrlBytes];
    if (request.path_is_sensitive) {
        redactUrlHostOnly(request.url, safe_url, sizeof(safe_url));
    } else {
        redactUrl(request.url, safe_url, sizeof(safe_url));
    }

    // ONE attempt, deliberately, and held for the WHOLE call rather than per-attempt as get()'s
    // loop does — see tlsGate()'s declaration in the header. Retrying a partial multi-megabyte
    // download from byte zero is exactly what a failure here already costs; a caller wanting to
    // retry the operation needs to reset the OTA partition write anyway (esp_ota_begin again), so
    // that decision belongs at the OtaService layer, not duplicated here.
    std::lock_guard<std::mutex> gate(tlsGate());
    return attemptStreamGet(request, safe_url, sink, response);
}

}  // namespace dashboard::net
