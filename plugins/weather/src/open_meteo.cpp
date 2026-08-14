#include "plugins/weather_provider.hpp"

#include <cmath>
#include <cstdio>

#include "esp_log.h"

namespace plugins {
namespace {

constexpr const char* kTag = "weather";

/// Room for the base URL, the variable lists and two formatted coordinates. The variable lists are
/// the bulk of it and are fixed at compile time, so this is generous rather than a guess.
constexpr size_t kUrlBytes = 640;

/// Two days, not one.
///
/// Today alone would leave the hourly strip empty through the evening — at 22:00 there is only one
/// entry left in a single-day request. Two days keeps six hours available at every hour of the day,
/// and costs about a kilobyte.
constexpr int kForecastDays = 2;

}  // namespace

bool OpenMeteoProvider::buildUrl(const WeatherQuery& query, char* out, size_t capacity) const {
    // Refuse coordinates that cannot be real before spending a TLS handshake on them. A corrupted
    // NVS value or a settings form submitted with an empty field lands here, and the API's error
    // response is far less legible in a footer than this is.
    if (!std::isfinite(query.latitude) || !std::isfinite(query.longitude) ||
        query.latitude < -90.0 || query.latitude > 90.0 || query.longitude < -180.0 ||
        query.longitude > 180.0) {
        return false;
    }

    // timezone=auto with timeformat=unixtime is the specific combination this parser expects: the
    // timestamps come back as true UTC epoch seconds, while the daily high/low buckets are cut on
    // the LOCAL calendar day at those coordinates. See the note in weather_model.hpp — the two
    // settings look redundant and are not.
    const int written = std::snprintf(
        out, capacity,
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,is_day,"
        "precipitation,weather_code,wind_speed_10m,wind_direction_10m"
        "&hourly=temperature_2m,precipitation_probability,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,"
        "sunrise,sunset"
        "&timezone=auto&timeformat=unixtime"
        "&temperature_unit=celsius&wind_speed_unit=kmh&precipitation_unit=mm"
        "&forecast_days=%d",
        query.latitude, query.longitude, kForecastDays);

    return written > 0 && static_cast<size_t>(written) < capacity;
}

esp_err_t OpenMeteoProvider::fetch(const WeatherQuery& query, WeatherData& out) {
    char url[kUrlBytes];
    if (!buildUrl(query, url, sizeof(url))) {
        last_error_ = "location is not valid";
        ESP_LOGW(kTag, "refusing to fetch for lat=%.4f lon=%.4f", query.latitude, query.longitude);
        return ESP_ERR_INVALID_ARG;
    }

    dashboard::net::HttpRequest request;
    request.url = url;

    // The previous body is invalidated before the request, not after a failure. Otherwise a failed
    // fetch would leave lastBody() pointing at a stale-but-plausible response that the plugin would
    // happily write back to the cache as if it were fresh.
    body_valid_ = false;
    body_length_ = 0;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, body_, sizeof(body_), response);
    if (err != ESP_OK) {
        // Distinguish the failures a person can act on. "Too big" in particular is ours to fix,
        // not the network's, and saying "offline" for it would send someone to check their router.
        if (response.truncated) {
            last_error_ = "forecast too large";
        } else if (response.status >= 400) {
            last_error_ = "forecast service refused the request";
        } else if (response.status >= 500) {
            last_error_ = "forecast service unavailable";
        } else {
            last_error_ = "could not reach the forecast service";
        }
        return err;
    }

    if (!parseOpenMeteo(body_, response.length, query.now_utc, out)) {
        last_error_ = "forecast response was not understood";
        ESP_LOGW(kTag, "parse failed on %u bytes", static_cast<unsigned>(response.length));
        return ESP_ERR_INVALID_RESPONSE;
    }

    body_length_ = response.length;
    body_valid_ = true;
    last_error_ = "";

    ESP_LOGI(kTag, "%.1f C, %s, %u hours ahead, %u bytes", out.temperature_c,
             wmoDescription(out.wmo_code), static_cast<unsigned>(out.hour_count),
             static_cast<unsigned>(response.length));
    return ESP_OK;
}

}  // namespace plugins
