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
#include <cstdint>
#include <functional>
#include <mutex>

#include "esp_err.h"

#include "app_config.hpp"

namespace dashboard::net {

/// One TLS session at a time, across the whole device. See https_client.cpp for the measured
/// reason this exists: two handshakes colliding fell the internal-SRAM low-water mark from
/// ~40 KB to 14 KB.
///
/// Exposed here (rather than kept file-local, as it was before streamGet() existed) because the
/// OTA download needs to hold it for its own entire multi-megabyte transfer, not just one
/// attempt — deliberately different from the per-ATTEMPT discipline get()'s retry loop uses. OTA
/// is rare and user-initiated, and letting a routine plugin fetch's handshake compete with it for
/// internal SRAM during a live flash write is exactly the class of bug that motivated this gate
/// in the first place.
std::mutex& tlsGate();

struct HttpRequest {
    /// Must be https://. Plain http:// is rejected rather than quietly downgraded.
    const char* url = nullptr;

    /// Sent as "Authorization: Bearer <token>". Never logged.
    const char* bearer = nullptr;

    /// One additional header, for APIs that want a key somewhere other than Authorization.
    /// The name may be logged; the value never is.
    const char* header_name = nullptr;
    const char* header_value = nullptr;

    /// When non-null, sent as a POST with this exact body and
    /// "Content-Type: application/x-www-form-urlencoded". GET otherwise.
    ///
    /// The caller builds the encoded string. There is exactly one family of callers so far — the
    /// Microsoft OAuth token endpoint — and every value going into that body (a client ID GUID, an
    /// opaque device code or refresh token) is already URL-safe by construction, so a general
    /// percent-encoder was not worth adding for one contract. If a future caller's values are not
    /// URL-safe, encode them before setting this field.
    const char* post_body = nullptr;

    /// Set when the URL's PATH ITSELF carries a credential, not just its query string.
    ///
    /// Every other integration here puts a credential in a header (GitHub, Microsoft) or a query
    /// parameter (TfL) — both covered by the ordinary log line, which keeps the path and drops
    /// only the query. Telegram's Bot API does neither: the token IS the path,
    /// `/bot<TOKEN>/getUpdates`, so that rule would print it. When this is set, the log line
    /// keeps only the scheme and host — see redactUrlHostOnly().
    bool path_is_sensitive = false;

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
    ///
    /// The same rules apply to a POST (`request.post_body` set): a non-2xx status still returns
    /// non-OK, but the body IS written to `out` first — callers that need to read an OAuth-style
    /// error body (`{"error":"authorization_pending",...}`) do so by inspecting `out` regardless
    /// of the return value, exactly as `response.status` is inspected regardless of it.
    esp_err_t get(const HttpRequest& request, char* out, size_t capacity, HttpResponse& response);

    /// A chunk of a streamed body. Return false to abort the transfer early — streamGet() then
    /// fails without reading any more, which is how a sink whose own write failed (a full flash
    /// partition, a hash context that cannot continue) stops the download rather than the caller
    /// discovering the problem only after the whole body has already been pointlessly fetched.
    using StreamSink = std::function<bool(const uint8_t* data, size_t len)>;

    /// Like get(), but the body is delivered to `sink` a chunk at a time instead of being
    /// accumulated into a caller buffer. For the one download in this firmware too large to hold
    /// in memory at once — the OTA image — everything else fits comfortably under
    /// kHttpMaxResponseBytes and should use get() instead.
    ///
    /// There is consequently NO SIZE CEILING here — the caller (OtaService) is the one place that
    /// both knows and enforces the expected size, from the signed manifest, and aborts via the
    /// sink's return value the moment more arrives than the manifest promised.
    ///
    /// `response.length` is the total byte count streamed, not a buffer occupancy; `response`'s
    /// `truncated` flag is unused here — a short body is instead a mismatch the caller detects by
    /// comparing `response.length` against the manifest's declared size.
    esp_err_t streamGet(const HttpRequest& request, const StreamSink& sink,
                        HttpResponse& response);
};

/// Copy `url` into `out`, stopping before any query string, for safe logging.
///
/// Exposed for tests and for callers that log their own request lines. See the header note: the
/// query is where credentials hide.
void redactUrl(const char* url, char* out, size_t capacity);

/// Copy only the scheme and host of `url` — no path, no query — for logging a request whose PATH
/// carries a credential. See HttpRequest::path_is_sensitive.
void redactUrlHostOnly(const char* url, char* out, size_t capacity);

}  // namespace dashboard::net
