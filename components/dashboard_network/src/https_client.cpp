#include "dashboard/net/https_client.hpp"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "version.hpp"

namespace dashboard::net {
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
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 3;

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

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "%s: connection failed: %s", safe_url, esp_err_to_name(err));
        // Wipe the Authorization header we built before the stack frame goes away.
        std::memset(auth, 0, sizeof(auth));
        esp_http_client_cleanup(client);
        return err;
    }
    std::memset(auth, 0, sizeof(auth));

    // 0 for a chunked response, whose length is genuinely unknown until it ends. The ceiling is
    // still enforced below, by the read loop.
    const int64_t content_length = esp_http_client_fetch_headers(client);
    response.status = esp_http_client_get_status_code(client);

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
    redactUrl(request.url, safe_url, sizeof(safe_url));

    const int attempts = (request.max_attempts > 0) ? request.max_attempts : 1;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 1; attempt <= attempts; ++attempt) {
        response = HttpResponse{};
        err = attemptGet(request, safe_url, out, capacity, response);
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

}  // namespace dashboard::net
