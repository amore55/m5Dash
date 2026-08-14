// The weather data the page displays, and the parser that produces it from an Open-Meteo response.
//
// Deliberately free of ESP-IDF *and* LVGL, like time_utils and json_util, so test/host can compile
// this exact source against saved fixtures. That is why parseOpenMeteo() takes `now_utc` as an
// argument instead of reading the clock: given the same bytes and the same instant it must always
// produce the same result, or a test cannot assert anything about which hours it selected.
//
// WHAT THE UPSTREAM ACTUALLY SENDS — established by querying the live API, not from the docs:
//
//   * Every timestamp is a TRUE UTC epoch second. The Open-Meteo documentation says daily
//     timestamps must have `utc_offset_seconds` added when a timezone is requested; that is not
//     what the service does. Verified against London on 2026-08-14: `sunrise` read back as
//     04:44 UTC, which is 05:44 BST — correct as-is, and an hour wrong if the offset were added.
//     So no correction is applied anywhere in this file.
//   * `timezone=auto` still matters, because it decides where the DAILY buckets are cut. With it,
//     `daily[0]` is the local calendar day at the requested coordinates (`daily.time[0]` came back
//     as local midnight expressed in UTC), so "today's high" means today where the weather is —
//     not a UTC day that starts at 01:00 during British Summer Time.
//   * A two-day request is ~2.2 KB with 48 hourly entries.
//
// Fields that upstream can omit or send as null (precipitation probability is the common one) are
// carried as sentinels rather than zero, because 0 % chance of rain and "not reported" should not
// render identically.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace plugins {

/// A coarse sky condition, collapsed from the WMO weather code.
///
/// The full WMO table has ~28 distinct values, several of which differ only by intensity. A desk
/// display has room for one glyph and two or three words, so the codes are grouped into the
/// classes a person would actually distinguish at a glance.
enum class Sky : uint8_t {
    Unknown,
    Clear,
    MostlyClear,
    PartlyCloudy,
    Overcast,
    Fog,
    Drizzle,
    Rain,
    FreezingRain,
    Snow,
    Showers,
    SnowShowers,
    Thunderstorm,
};

/// Map a WMO 4677-style code onto a Sky. Unrecognised codes yield Unknown rather than a guess:
/// showing "Unknown" is honest, and inventing "Clear" from a code we do not know is not.
Sky skyFromWmoCode(int32_t code);

/// Short, sentence-case description of the class: "Partly cloudy", "Rain".
const char* skyDescription(Sky sky);

/// Sentence-case description of the exact code, intensity included: "Heavy rain showers".
///
/// Sky deliberately discards intensity so that the hourly chips can be labelled consistently; the
/// current-conditions headline has room for the real thing, and "Heavy rain" is materially
/// different information from "Rain". An unrecognised code gives "Unknown".
const char* wmoDescription(int32_t code);

/// Sentinel for "upstream did not report this". Chosen negative so it cannot be confused with a
/// real percentage or a real millimetre reading.
constexpr int32_t kNoValue = -1;

struct WeatherHour {
    std::time_t time_utc = 0;
    double temperature_c = 0.0;
    int32_t precipitation_probability = kNoValue;
    int32_t wmo_code = kNoValue;
    Sky sky = Sky::Unknown;
};

struct WeatherData {
    /// False until a response has been parsed successfully. The page checks this rather than
    /// inspecting individual fields, so a half-populated model can never be drawn.
    bool valid = false;

    // ---- current conditions -------------------------------------------------------------
    std::time_t observed_utc = 0;
    double temperature_c = 0.0;
    double apparent_c = 0.0;
    double wind_kph = 0.0;
    int32_t wind_direction_deg = kNoValue;
    int32_t humidity_percent = kNoValue;
    double precipitation_mm = 0.0;
    int32_t wmo_code = kNoValue;
    Sky sky = Sky::Unknown;
    bool is_day = true;

    // ---- today ---------------------------------------------------------------------------
    bool has_daily = false;
    double high_c = 0.0;
    double low_c = 0.0;
    int32_t rain_probability_percent = kNoValue;
    std::time_t sunrise_utc = 0;
    std::time_t sunset_utc = 0;

    // ---- the next few hours --------------------------------------------------------------
    /// Six is what fits across the width of the card at a readable size, and six hours is about
    /// as far ahead as an hourly forecast is worth trusting.
    static constexpr size_t kMaxHours = 6;
    WeatherHour hours[kMaxHours];
    size_t hour_count = 0;

    /// Compass point for wind_direction_deg: "NE", "SSW", or "" when not reported.
    const char* windCompass() const;
};

/// Parse an Open-Meteo `/v1/forecast` response.
///
/// `now_utc` selects which hourly entries count as "next": the first entry strictly after it, and
/// the five following. Pass 0 to take the first available entries instead, which is what a caller
/// without a valid clock should do.
///
/// Returns false only when the body is not JSON or carries no current conditions at all. A missing
/// `daily` or `hourly` block is a partial success: `out.valid` is true, `has_daily`/`hour_count`
/// say what arrived. Half a forecast is worth displaying; refusing it would blank the page because
/// one optional block was absent.
bool parseOpenMeteo(const char* json, size_t len, std::time_t now_utc, WeatherData& out);

}  // namespace plugins
