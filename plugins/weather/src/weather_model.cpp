#include "plugins/weather_model.hpp"

#include "dashboard/json_util.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;

/// Read element `index` of a numeric array into an int32, leaving the "not reported" sentinel in
/// place if it is absent or null.
///
/// Open-Meteo pads hourly arrays with `null` wherever a variable is unavailable for a location, so
/// a failed read here is a normal outcome and not an error.
void arrayInt(const cJSON* array_node, size_t index, int32_t& out) {
    double value = 0.0;
    if (json::numberAt(array_node, index, value)) {
        out = static_cast<int32_t>(value);
    }
}

/// Pick the first hourly index strictly after `now_utc`.
///
/// "Strictly after" because the entry stamped 14:00 describes the hour that has already started;
/// showing it as the *next* hour when it is 14:30 would be a forecast for the recent past. When the
/// clock is unknown (now_utc == 0) or every entry is in the past, start at 0 — for a device that
/// has not synced yet, the beginning of the array is the most defensible choice.
size_t firstFutureHour(const cJSON* times, size_t count, std::time_t now_utc) {
    if (now_utc <= 0) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        double stamp = 0.0;
        if (!json::numberAt(times, i, stamp)) {
            continue;
        }
        if (static_cast<std::time_t>(stamp) > now_utc) {
            return i;
        }
    }
    return 0;
}

}  // namespace

Sky skyFromWmoCode(int32_t code) {
    switch (code) {
        case 0:
            return Sky::Clear;
        case 1:
            return Sky::MostlyClear;
        case 2:
            return Sky::PartlyCloudy;
        case 3:
            return Sky::Overcast;
        case 45:
        case 48:
            return Sky::Fog;
        case 51:
        case 53:
        case 55:
            return Sky::Drizzle;
        // Freezing drizzle groups with freezing rain, not with drizzle: the thing that matters
        // about it is the ice, and the intensity is beside the point.
        case 56:
        case 57:
        case 66:
        case 67:
            return Sky::FreezingRain;
        case 61:
        case 63:
        case 65:
            return Sky::Rain;
        case 71:
        case 73:
        case 75:
        case 77:
            return Sky::Snow;
        case 80:
        case 81:
        case 82:
            return Sky::Showers;
        case 85:
        case 86:
            return Sky::SnowShowers;
        case 95:
        case 96:
        case 99:
            return Sky::Thunderstorm;
        default:
            return Sky::Unknown;
    }
}

const char* skyDescription(Sky sky) {
    switch (sky) {
        case Sky::Clear:
            return "Clear";
        case Sky::MostlyClear:
            return "Mostly clear";
        case Sky::PartlyCloudy:
            return "Partly cloudy";
        case Sky::Overcast:
            return "Overcast";
        case Sky::Fog:
            return "Fog";
        case Sky::Drizzle:
            return "Drizzle";
        case Sky::Rain:
            return "Rain";
        case Sky::FreezingRain:
            return "Freezing rain";
        case Sky::Snow:
            return "Snow";
        case Sky::Showers:
            return "Showers";
        case Sky::SnowShowers:
            return "Snow showers";
        case Sky::Thunderstorm:
            return "Thunderstorm";
        case Sky::Unknown:
        default:
            return "Unknown";
    }
}

const char* wmoDescription(int32_t code) {
    switch (code) {
        case 0:
            return "Clear sky";
        case 1:
            return "Mainly clear";
        case 2:
            return "Partly cloudy";
        case 3:
            return "Overcast";
        case 45:
            return "Fog";
        case 48:
            return "Freezing fog";
        case 51:
            return "Light drizzle";
        case 53:
            return "Drizzle";
        case 55:
            return "Heavy drizzle";
        case 56:
            return "Light freezing drizzle";
        case 57:
            return "Freezing drizzle";
        case 61:
            return "Light rain";
        case 63:
            return "Rain";
        case 65:
            return "Heavy rain";
        case 66:
            return "Light freezing rain";
        case 67:
            return "Freezing rain";
        case 71:
            return "Light snow";
        case 73:
            return "Snow";
        case 75:
            return "Heavy snow";
        case 77:
            return "Snow grains";
        case 80:
            return "Light rain showers";
        case 81:
            return "Rain showers";
        case 82:
            return "Heavy rain showers";
        case 85:
            return "Light snow showers";
        case 86:
            return "Snow showers";
        case 95:
            return "Thunderstorm";
        case 96:
            return "Thunderstorm with hail";
        case 99:
            return "Thunderstorm with heavy hail";
        default:
            return "Unknown";
    }
}

const char* WeatherData::windCompass() const {
    if (wind_direction_deg == kNoValue) {
        return "";
    }
    static const char* const kPoints[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                          "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    // Sixteen sectors of 22.5 degrees, rounded to nearest in integer arithmetic: multiplying by 4
    // clears the fraction, and +45 over 90 is the round-half-up. The trailing modulo folds 348.75
    // degrees and above back into N instead of running off the end of the table.
    const int32_t normalised = ((wind_direction_deg % 360) + 360) % 360;
    const size_t index = static_cast<size_t>(((normalised * 4 + 45) / 90) % 16);
    return kPoints[index];
}

bool parseOpenMeteo(const char* json_text, size_t len, std::time_t now_utc, WeatherData& out) {
    out = WeatherData{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    // ---- current conditions. The only mandatory block. -----------------------------------
    const cJSON* current = json::object(doc.root(), "current");
    if (current == nullptr) {
        return false;
    }

    double stamp = 0.0;
    if (json::number(current, "time", stamp)) {
        out.observed_utc = static_cast<std::time_t>(stamp);
    }
    // Temperature is the one field whose absence makes the page pointless.
    if (!json::number(current, "temperature_2m", out.temperature_c)) {
        return false;
    }
    out.apparent_c = out.temperature_c;  // sensible fallback if apparent_temperature is absent
    json::number(current, "apparent_temperature", out.apparent_c);
    json::number(current, "wind_speed_10m", out.wind_kph);
    json::integer(current, "wind_direction_10m", out.wind_direction_deg);
    json::integer(current, "relative_humidity_2m", out.humidity_percent);
    json::number(current, "precipitation", out.precipitation_mm);
    json::integer(current, "weather_code", out.wmo_code);
    out.sky = skyFromWmoCode(out.wmo_code);

    // is_day arrives as 0/1 rather than a JSON boolean, so it is read as a number. Defaults to
    // daytime: a wrong night icon on a sunny afternoon is the more jarring of the two errors.
    int32_t is_day = 1;
    json::integer(current, "is_day", is_day);
    out.is_day = (is_day != 0);

    // ---- today's daily block. Optional. ---------------------------------------------------
    const cJSON* daily = json::object(doc.root(), "daily");
    if (daily != nullptr) {
        const cJSON* highs = json::array(daily, "temperature_2m_max");
        const cJSON* lows = json::array(daily, "temperature_2m_min");
        double high = 0.0;
        double low = 0.0;
        // Both bounds or neither: a card showing a high with no low reads as a data fault, and
        // there is no honest way to fill in the missing half.
        if (json::numberAt(highs, 0, high) && json::numberAt(lows, 0, low)) {
            out.high_c = high;
            out.low_c = low;
            out.has_daily = true;
        }
        arrayInt(json::array(daily, "precipitation_probability_max"), 0,
                 out.rain_probability_percent);

        double sunrise = 0.0;
        if (json::numberAt(json::array(daily, "sunrise"), 0, sunrise)) {
            out.sunrise_utc = static_cast<std::time_t>(sunrise);
        }
        double sunset = 0.0;
        if (json::numberAt(json::array(daily, "sunset"), 0, sunset)) {
            out.sunset_utc = static_cast<std::time_t>(sunset);
        }
    }

    // ---- the next few hours. Optional. ----------------------------------------------------
    const cJSON* hourly = json::object(doc.root(), "hourly");
    if (hourly != nullptr) {
        const cJSON* times = json::array(hourly, "time");
        const cJSON* temps = json::array(hourly, "temperature_2m");
        const cJSON* probabilities = json::array(hourly, "precipitation_probability");
        const cJSON* codes = json::array(hourly, "weather_code");

        const size_t count = json::arraySize(times);
        const size_t start = firstFutureHour(times, count, now_utc);

        for (size_t i = start; i < count && out.hour_count < WeatherData::kMaxHours; ++i) {
            double when = 0.0;
            double temperature = 0.0;
            // An hour without a time or a temperature is skipped rather than defaulted: a chip
            // reading "0 °C" for a gap in the data would be a lie, and a blank chip is noise.
            if (!json::numberAt(times, i, when) || !json::numberAt(temps, i, temperature)) {
                continue;
            }
            WeatherHour& hour = out.hours[out.hour_count++];
            hour.time_utc = static_cast<std::time_t>(when);
            hour.temperature_c = temperature;
            arrayInt(probabilities, i, hour.precipitation_probability);
            arrayInt(codes, i, hour.wmo_code);
            hour.sky = skyFromWmoCode(hour.wmo_code);
        }
    }

    out.valid = true;
    return true;
}

}  // namespace plugins
