// The seam between "what the weather page shows" and "who told us".
//
// The brief asks for Open-Meteo *behind an interface*, and the reason is not hypothetical
// multi-provider support: it is that WeatherPlugin then has no HTTP or JSON in it at all. The page
// asks for a WeatherData and gets one, so the UI can be reasoned about — and later a mock provider
// can drive it — without a network stack in the picture.
//
// Open-Meteo needs no API key for non-commercial use, which is why it is the default and why the
// plugin has no "not configured" state: a fresh device shows real weather for the default London
// coordinates immediately.

#pragma once

#include <ctime>

#include "esp_err.h"

#include "dashboard/net/https_client.hpp"
#include "plugins/weather_model.hpp"

namespace plugins {

/// Where to fetch for. Grouped into a struct so adding a unit preference later does not change
/// every signature.
struct WeatherQuery {
    double latitude = 51.5072;
    double longitude = -0.1276;
    /// Current UTC time, or 0 if the clock is not yet valid. Passed in rather than read inside so
    /// the hour selection stays deterministic and testable — see parseOpenMeteo().
    std::time_t now_utc = 0;
};

class WeatherProvider {
  public:
    virtual ~WeatherProvider() = default;

    /// Fetch and parse. **Worker thread.** Returns ESP_OK only when `out.valid` is true.
    virtual esp_err_t fetch(const WeatherQuery& query, WeatherData& out) = 0;

    /// A short, user-facing reason for the last failure — rendered in the page footer, so it must
    /// never contain a URL with a query string or anything credential-shaped.
    virtual const char* lastError() const = 0;

    /// The raw body of the last successful fetch, so the plugin can cache it. Returns nullptr when
    /// nothing has succeeded yet.
    ///
    /// The plugin caches the RESPONSE rather than the parsed struct: a firmware update that adds a
    /// field can then read new information out of yesterday's cached body, and there is no second
    /// serialisation format to keep in step with WeatherData.
    virtual const char* lastBody() const = 0;
    virtual size_t lastBodyLength() const = 0;
};

/// Open-Meteo's free `/v1/forecast` endpoint.
class OpenMeteoProvider final : public WeatherProvider {
  public:
    esp_err_t fetch(const WeatherQuery& query, WeatherData& out) override;

    const char* lastError() const override { return last_error_; }
    const char* lastBody() const override { return body_valid_ ? body_ : nullptr; }
    size_t lastBodyLength() const override { return body_valid_ ? body_length_ : 0; }

    /// Parse a body this provider did not fetch — the cached copy from the previous boot.
    /// Static because it needs none of the instance's state, and being static makes it obvious at
    /// the call site that no network access happens.
    static bool parseCached(const char* body, size_t len, std::time_t now_utc, WeatherData& out) {
        return parseOpenMeteo(body, len, now_utc, out);
    }

    /// Ceiling on the response buffer.
    ///
    /// A two-day, three-variable hourly request measured 2191 bytes against the live API. 6 KB is
    /// ample headroom for longer coordinate precision and any field Open-Meteo adds, while staying
    /// small enough to sit in the plugin object rather than on the worker's stack.
    static constexpr size_t kResponseBytes = 6144;

  private:
    /// Built fresh per fetch. Coordinates come from settings and are formatted with %.4f — four
    /// decimal places is ~11 m, far beyond what a weather grid resolves, and it keeps the URL a
    /// predictable length.
    bool buildUrl(const WeatherQuery& query, char* out, size_t capacity) const;

    dashboard::net::HttpsClient http_;

    char body_[kResponseBytes] = {};
    size_t body_length_ = 0;
    bool body_valid_ = false;

    const char* last_error_ = "";
};

}  // namespace plugins
