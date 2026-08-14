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

    /// Fetch and parse. **Worker thread.**
    ///
    /// The CALLER supplies the buffer, and on success `body_length` reports how much of it holds
    /// the raw response — which is what the plugin writes to the cache. It caches the RESPONSE
    /// rather than the parsed struct so that a firmware update adding a field can read new
    /// information out of yesterday's body, with no second serialisation format to maintain.
    ///
    /// Buffer ownership sits with the caller because a provider-owned array would be a member of a
    /// statically allocated plugin, and therefore permanently resident in internal SRAM. See
    /// dashboard/net/response_buffer.hpp.
    virtual esp_err_t fetch(const WeatherQuery& query, char* buffer, size_t capacity,
                            WeatherData& out, size_t& body_length) = 0;

    /// A short, user-facing reason for the last failure — rendered in the page footer, so it must
    /// never contain a URL with a query string or anything credential-shaped.
    virtual const char* lastError() const = 0;
};

/// Open-Meteo's free `/v1/forecast` endpoint.
class OpenMeteoProvider final : public WeatherProvider {
  public:
    esp_err_t fetch(const WeatherQuery& query, char* buffer, size_t capacity, WeatherData& out,
                    size_t& body_length) override;

    const char* lastError() const override { return last_error_; }

    /// Parse a body this provider did not fetch — the cached copy from the previous boot.
    /// Static because it needs none of the instance's state, and being static makes it obvious at
    /// the call site that no network access happens.
    static bool parseCached(const char* body, size_t len, std::time_t now_utc, WeatherData& out) {
        return parseOpenMeteo(body, len, now_utc, out);
    }

    /// How much the caller should allocate.
    ///
    /// A two-day, three-variable hourly request measured 2191 bytes against the live API. 6 KB is
    /// ample headroom for longer coordinate precision and any field Open-Meteo adds.
    static constexpr size_t kResponseBytes = 6144;

  private:
    /// Built fresh per fetch. Coordinates come from settings and are formatted with %.4f — four
    /// decimal places is ~11 m, far beyond what a weather grid resolves, and it keeps the URL a
    /// predictable length.
    bool buildUrl(const WeatherQuery& query, char* out, size_t capacity) const;

    dashboard::net::HttpsClient http_;

    const char* last_error_ = "";
};

}  // namespace plugins
