// The one way this firmware talks to the internet.
//
// Every upstream integration — weather, the Elizabeth line, Telegram, Claude usage, the OTA
// manifest — goes through here, so the properties below are guaranteed once rather than
// re-argued per plugin.
//
// WHAT IT GUARANTEES
//
//   * TLS with real certificate validation, against ESP-IDF's bundled root store
//     (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE). There is no "skip verification" option, deliberately:
//     an escape hatch for one awkward host is how a fleet ends up trusting anything.
//   * A hard ceiling on the response. The caller supplies the buffer, so a hostile or broken
//     upstream cannot make the device allocate until it dies. A body that does not fit is
//     truncated and SAID to be truncated, never silently half-parsed.
//   * A timeout on every request, and bounded retries with backoff for failures worth retrying.
//   * Nothing sensitive in the log. See below — this is the part that is easy to get wrong.
//
// LOGGING, AND WHY THE URL IS NOT LOGGED WHOLE
//
// Credentials do not only live in headers. TfL takes an app key as a QUERY PARAMETER, so logging
// a full request URL would put a live credential in the serial log and in any bug report pasted
// from it. Every log line here is therefore built from the scheme, host and path only, and the
// query is dropped. Header values are never logged at all, at any level.
//
// THREADING
//
// Each call builds and tears down its own esp_http_client, so there is no shared state and
// concurrent calls from different plugin workers are fine. Calls BLOCK — for as long as the
// timeout allows — so they belong on a worker task, never on the LVGL thread.

#pragma once

#include <cstddef>

#include "esp_err.h"

#include "app_config.hpp"

namespace dashboard::net {

struct HttpRequest {
    /// Must be https://. Plain http:// is rejected rather than quietly downgraded.
    const char* url = nullptr;

    /// Sent as "Authorization: Bearer <token>". Never logged.
    const char* bearer = nullptr;

    /// One additional header, for APIs that want a key somewhere other than Authorization.
    /// The name may be logged; the value never is.
    const char* header_name = nullptr;
    const char* header_value = nullptr;

    int timeout_ms = dash::cfg::kHttpTimeoutMs;
    int max_attempts = dash::cfg::kHttpMaxAttempts;
};

struct HttpResponse {
    /// HTTP status of the final response, after any redirects. 0 if the request never completed.
    int status = 0;

    /// Bytes written to the caller's buffer, excluding the terminator.
    size_t length = 0;

    /// The body was longer than the buffer and has been cut short.
    ///
    /// Callers must treat a truncated body as a failure rather than parsing it: a JSON document
    /// missing its tail is not a smaller document, it is a broken one.
    bool truncated = false;
};

class HttpsClient {
  public:
    /// Perform a GET, writing the body into `out` and NUL-terminating it.
    ///
    /// Returns ESP_OK only when the request completed and the status was 2xx. A non-2xx status is
    /// reported through `response.status` AND a non-OK return, so a caller cannot accidentally
    /// parse an error page by checking only one of the two.
    ///
    /// `capacity` must be at least 2. Anything above dash::cfg::kHttpMaxResponseBytes is capped
    /// to it — the ceiling is the component's promise, not the caller's choice.
    esp_err_t get(const HttpRequest& request, char* out, size_t capacity, HttpResponse& response);
};

/// Copy `url` into `out`, stopping before any query string, for safe logging.
///
/// Exposed for tests and for callers that log their own request lines. See the header note: the
/// query is where credentials hide.
void redactUrl(const char* url, char* out, size_t capacity);

}  // namespace dashboard::net
