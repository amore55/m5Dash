#include "plugins/weather_plugin.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/storage/cache_store.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace theme = dashboard::theme;
namespace timeutil = dashboard::timeutil;
using dashboard::storage::CacheStore;

constexpr const char* kTag = "weather";

/// CacheStore key. Also the plugin id, which is deliberate: one namespace, no second thing to keep
/// in step.
constexpr const char* kCacheKey = "weather";

/// 250 % of the 48 px hero face is ~120 px digits. Two glyphs and a degree sign at that size fill
/// the left half of the page, which is the point: temperature is the one value read from across the
/// room. Kept well under the ~400 % where the scaled bitmap turns soft.
constexpr int32_t kTemperatureScale = theme::heroScalePercent(250);

constexpr int32_t kDetailCardWidth = 420;

/// Four stacked lines at 16/28/14/14 px come to about 112 px including the row gaps. 170 px with
/// the reduced chip padding below leaves real headroom; the card default of kGapL all round left
/// the strip two pixels short of its own content, which clips silently because these containers
/// have scrolling disabled.
constexpr int32_t kHourStripHeight = 170;
constexpr int32_t kHourChipPadding = theme::kGapM;

/// Rain probabilities below this are noise in a forecast and clutter on a chip.
constexpr int32_t kRainWorthShowing = 5;

/// Shown wherever a value is not known. One consistent placeholder, so a missing field reads as
/// missing rather than as a layout fault.
constexpr const char* kNoData = "--";

/// Row order in the details card. An enum rather than bare indices so that inserting a row cannot
/// silently shift every value into the wrong line.
enum DetailRow : size_t {
    kRowHighLow = 0,
    kRowFeelsLike,
    kRowRain,
    kRowWind,
    kRowHumidity,
    kRowSun,
};

/// Round to a whole degree for display.
///
/// std::lround rather than a cast, which truncates: 28.8 must read 29, not 28. It also removes the
/// "-0" that %.0f produces for a temperature just below freezing point of a degree.
int wholeDegrees(double celsius) { return static_cast<int>(std::lround(celsius)); }

/// Format an epoch second as local "HH:MM". Writes kNoData for a zero or unconvertible stamp.
///
/// Local, not UTC: every timestamp in the model is UTC by design, and every one of them is read by
/// a person standing in front of the device.
void formatLocalTime(char* out, size_t capacity, std::time_t when) {
    std::tm local = {};
    if (when <= 0 || localtime_r(&when, &local) == nullptr) {
        std::snprintf(out, capacity, "%s", kNoData);
        return;
    }
    timeutil::formatTime24h(out, capacity, local, /*with_seconds=*/false);
}

}  // namespace

WeatherPlugin::WeatherPlugin() : PluginBase("weather", "Weather") {}

uint32_t WeatherPlugin::refreshIntervalMs() const { return dash::cfg::kWeatherRefreshMs; }

// ---------------------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------------------

esp_err_t WeatherPlugin::onInitialise() {
    loadCachedForecast();
    // Always usable: Open-Meteo needs no key, so unlike the Claude or Telegram pages there is no
    // configuration state in which this plugin has to disable itself.
    return ESP_OK;
}

void WeatherPlugin::loadCachedForecast() {
    if (!CacheStore::has(kCacheKey)) {
        return;
    }

    // PSRAM, not the stack: this runs on the application start-up task, whose stack is nowhere near
    // 6 KB spare. Released when the buffer goes out of scope at the end of this function.
    dashboard::net::ResponseBuffer buffer(OpenMeteoProvider::kResponseBytes);
    if (!buffer.valid()) {
        ESP_LOGW(kTag, "no memory to read the cached forecast");
        return;
    }

    size_t length = 0;
    const esp_err_t err = CacheStore::get(kCacheKey, buffer.data(), buffer.capacity(), &length);
    if (err == ESP_OK && length > 0) {
        // The clock has already been restored from the RTC by this point in the boot sequence, so
        // "which hours are still in the future" is usually answerable. If it is not, parseOpenMeteo
        // takes the first hours in the file and the next refresh corrects it.
        const std::time_t now = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;
        WeatherData cached;
        if (OpenMeteoProvider::parseCached(buffer.data(), length, now, cached)) {
            {
                std::lock_guard<std::mutex> lock(modelMutex());
                data_ = cached;
            }
            // Tell the state machine the page is showing real, old data — otherwise a failed first
            // refresh would report "Unavailable" over a screen full of numbers.
            noteCachedData(static_cast<std::time_t>(CacheStore::timestamp(kCacheKey)));
            ESP_LOGI(kTag, "restored a cached forecast (%u bytes)", static_cast<unsigned>(length));
        } else {
            // A cache entry that no longer parses is worse than none: it would be re-read on every
            // boot and fail every time.
            ESP_LOGW(kTag, "cached forecast did not parse; discarding it");
            CacheStore::remove(kCacheKey);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void WeatherPlugin::buildBody(lv_obj_t* body) {
    lv_obj_t* top = theme::makeRow(body);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_flex_grow(top, 1);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, theme::kGapXl, LV_PART_MAIN);

    buildCurrentBlock(top);
    buildDetailsCard(top);
    buildHourStrip(body);
}

void WeatherPlugin::buildCurrentBlock(lv_obj_t* parent) {
    lv_obj_t* column = theme::makeColumn(parent);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    // Centred, and not only for looks: the temperature is drawn by an LVGL transform, which
    // enlarges the glyphs beyond the label's laid-out box without enlarging the box. In a
    // left-aligned column the overflow would be clipped by the container edge; centred in a
    // 700-odd pixel column there is room to spare on both sides.
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(column, theme::kGapS, LV_PART_MAIN);

    place_ = theme::makeLabel(column, "", theme::fontTitle(), theme::textSecondary());

    temperature_ = theme::makeLabel(column, kNoData, theme::fontHero(), theme::textPrimary());
    theme::applyHeroScale(temperature_, kTemperatureScale);
    // Vertical room for the same overflow: at 250 % a 48 px line draws about 145 px tall out of a
    // 58 px box, so roughly 44 px escapes above and below and needs absorbing on both sides.
    lv_obj_set_style_pad_ver(temperature_, theme::kGapXl + theme::kGapM, LV_PART_MAIN);

    condition_ = theme::makeLabel(column, "", theme::fontDisplay(), theme::textPrimary());
}

lv_obj_t* WeatherPlugin::addDetailRow(lv_obj_t* parent, const char* name, bool separator_above) {
    if (separator_above) {
        theme::makeSeparator(parent);
    }
    lv_obj_t* row = theme::makeRow(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    // Name left, value hard right: a consistent right edge is what makes a column of unrelated
    // values scannable.
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // One step up the type scale from where these started (label/body): at desk distance the
    // right-hand column was legible but not comfortable, and the card has the room because its
    // height is LV_SIZE_CONTENT.
    theme::makeLabel(row, name, theme::fontBody(), theme::textMuted());
    return theme::makeLabel(row, kNoData, theme::fontTitle(), theme::textPrimary());
}

void WeatherPlugin::buildDetailsCard(lv_obj_t* parent) {
    lv_obj_t* card = theme::makeCard(parent);
    lv_obj_set_width(card, kDetailCardWidth);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, theme::kGapS, LV_PART_MAIN);

    detail_values_[kRowHighLow] = addDetailRow(card, "High / low", false);
    detail_values_[kRowFeelsLike] = addDetailRow(card, "Feels like", true);
    detail_values_[kRowRain] = addDetailRow(card, "Rain today", true);
    detail_values_[kRowWind] = addDetailRow(card, "Wind", true);
    detail_values_[kRowHumidity] = addDetailRow(card, "Humidity", true);
    detail_values_[kRowSun] = addDetailRow(card, "Sunrise / sunset", true);
}

void WeatherPlugin::buildHourStrip(lv_obj_t* parent) {
    lv_obj_t* strip = theme::makeRow(parent);
    lv_obj_set_width(strip, LV_PCT(100));
    lv_obj_set_height(strip, kHourStripHeight);
    lv_obj_set_style_pad_column(strip, theme::kGapM, LV_PART_MAIN);

    for (size_t i = 0; i < WeatherData::kMaxHours; ++i) {
        HourChip& chip = hours_[i];
        chip.root = theme::makeCard(strip);
        // Equal shares of the width rather than six fixed sizes, so the strip stays correct if the
        // gutter or the hour count changes.
        lv_obj_set_flex_grow(chip.root, 1);
        lv_obj_set_height(chip.root, LV_PCT(100));
        lv_obj_set_style_pad_all(chip.root, kHourChipPadding, LV_PART_MAIN);
        lv_obj_set_flex_flow(chip.root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(chip.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(chip.root, theme::kGapXs, LV_PART_MAIN);

        chip.time = theme::makeLabel(chip.root, "", theme::fontLabel(), theme::textSecondary());
        chip.temperature =
            theme::makeLabel(chip.root, kNoData, theme::fontTitle(), theme::textPrimary());
        chip.sky = theme::makeLabel(chip.root, "", theme::fontMicro(), theme::textMuted());
        // Rain sits in the accent colour because it is the one number on the chip that changes a
        // decision: it is the difference between taking a coat and not.
        chip.rain = theme::makeLabel(chip.root, "", theme::fontMicro(), theme::accent());
    }
}

// ---------------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------------

void WeatherPlugin::setLocation(double latitude, double longitude, const char* label) {
    bool moved = false;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        // Exact comparison is right here. These arrive from NVS as fixed-point micro-degrees and
        // round-trip identically, so any difference at all is a real edit rather than float noise.
        moved = (latitude != latitude_) || (longitude != longitude_);
        latitude_ = latitude;
        longitude_ = longitude;
        label_.assign(label);
        if (moved) {
            // Drop the old place's forecast rather than show it under the new heading.
            data_ = WeatherData{};
        }
    }

    markDirty();
    if (moved) {
        ESP_LOGI(kTag, "location changed to %.4f, %.4f (%s)", latitude, longitude, label);
        // Do not wait out the rest of a 20 minute interval to reflect a setting the user just
        // changed. refresh() coalesces, so a burst of edits cannot pile up fetches.
        refresh(/*force=*/true);
    }
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t WeatherPlugin::fetch(bool force) {
    (void)force;  // there is nothing cheaper than a full fetch to skip

    WeatherQuery query;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        query.latitude = latitude_;
        query.longitude = longitude_;
    }
    // Outside the lock, and deliberately: the model mutex must not be held across a blocking
    // network call, or setLocation() on the LVGL thread would stall for the whole HTTP timeout.
    query.now_utc = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;

    // From PSRAM, and released when this returns. A member array would live in internal SRAM for
    // the life of the device — see dashboard/net/response_buffer.hpp and docs/BACKLOG.md §1.3.
    dashboard::net::ResponseBuffer buffer(OpenMeteoProvider::kResponseBytes);
    if (!buffer.valid()) {
        setError("out of memory");
        return ESP_ERR_NO_MEM;
    }

    WeatherData fresh;
    size_t body_length = 0;
    const esp_err_t err =
        provider_.fetch(query, buffer.data(), buffer.capacity(), fresh, body_length);
    if (err != ESP_OK) {
        setError(provider_.lastError());
        return err;
    }

    {
        std::lock_guard<std::mutex> lock(modelMutex());
        data_ = fresh;
    }

    // Cache the raw body while the buffer is still in scope. A cache write failure is logged and
    // otherwise ignored: the fetch itself succeeded, and failing the refresh over it would replace
    // good data on screen with an error.
    const esp_err_t cache_err = CacheStore::put(kCacheKey, buffer.data(), body_length);
    if (cache_err != ESP_OK) {
        ESP_LOGW(kTag, "could not cache the forecast: %s", esp_err_to_name(cache_err));
    }

    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------

void WeatherPlugin::summarise(dashboard::PluginSummary& out) const {
    WeatherData data;
    dashboard::MediumString label;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        data = data_;
        label = label_;
    }

    if (!data.valid) {
        out.primary.assign(kNoData);
        out.secondary.assign(label.c_str());
        return;
    }

    char text[64];
    // Degree sign only — no trailing C. The tile is narrow and the unit never changes, so the
    // letter is the first thing worth losing. 0xB0 is in the glyph range; anything outside it
    // renders as an empty box.
    std::snprintf(text, sizeof(text), "%d°", wholeDegrees(data.temperature_c));
    out.primary.assign(text);

    // Prefer TfL-style specific wording over the coarse Sky bucket: wmoDescription() knows
    // "Partly cloudy" where skyDescription() would only say "Cloudy".
    const char* words = (data.wmo_code != kNoValue) ? wmoDescription(data.wmo_code)
                                                    : skyDescription(data.sky);
    std::snprintf(text, sizeof(text), "%s • %s", words, label.c_str());
    out.secondary.assign(text);
}

void WeatherPlugin::updateUi() {
    // Snapshot, then render outside the lock. WeatherData is a few hundred bytes of plain data, so
    // the copy is cheaper than holding the model mutex across a few dozen LVGL calls — and it means
    // a fetch completing mid-render cannot change values half way down the page.
    WeatherData data;
    char label[dashboard::MediumString::capacity()];
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        data = data_;
        std::snprintf(label, sizeof(label), "%s", label_.c_str());
    }

    if (place_ != nullptr) {
        lv_label_set_text(place_, label);
    }
    renderCurrent(data);
    renderDetails(data);
    renderHours(data);
}

void WeatherPlugin::renderCurrent(const WeatherData& data) {
    if (temperature_ == nullptr || condition_ == nullptr) {
        return;
    }

    if (!data.valid) {
        lv_label_set_text(temperature_, kNoData);
        lv_label_set_text(condition_, "");
        return;
    }

    char text[16];
    std::snprintf(text, sizeof(text), "%d\xC2\xB0", wholeDegrees(data.temperature_c));
    lv_label_set_text(temperature_, text);
    lv_label_set_text(condition_, wmoDescription(data.wmo_code));
}

void WeatherPlugin::renderDetails(const WeatherData& data) {
    for (lv_obj_t* value : detail_values_) {
        if (value == nullptr) {
            return;  // body not built yet
        }
    }

    if (!data.valid) {
        for (lv_obj_t* value : detail_values_) {
            lv_label_set_text(value, kNoData);
        }
        return;
    }

    char text[48];

    if (data.has_daily) {
        std::snprintf(text, sizeof(text), "%d\xC2\xB0 / %d\xC2\xB0", wholeDegrees(data.high_c),
                      wholeDegrees(data.low_c));
    } else {
        std::snprintf(text, sizeof(text), "%s", kNoData);
    }
    lv_label_set_text(detail_values_[kRowHighLow], text);

    // One decimal place here, against whole degrees in the hero: this is a comparison against the
    // real temperature, and rounding both to integers would show "29 / 29" for a real difference.
    std::snprintf(text, sizeof(text), "%.1f\xC2\xB0", data.apparent_c);
    lv_label_set_text(detail_values_[kRowFeelsLike], text);

    if (data.rain_probability_percent == kNoValue) {
        std::snprintf(text, sizeof(text), "%s", kNoData);
    } else {
        std::snprintf(text, sizeof(text), "%ld%%",
                      static_cast<long>(data.rain_probability_percent));
    }
    lv_label_set_text(detail_values_[kRowRain], text);

    const char* compass = data.windCompass();
    std::snprintf(text, sizeof(text), "%d km/h%s%s", static_cast<int>(std::lround(data.wind_kph)),
                  compass[0] != '\0' ? " " : "", compass);
    lv_label_set_text(detail_values_[kRowWind], text);

    if (data.humidity_percent == kNoValue) {
        std::snprintf(text, sizeof(text), "%s", kNoData);
    } else {
        std::snprintf(text, sizeof(text), "%ld%%", static_cast<long>(data.humidity_percent));
    }
    lv_label_set_text(detail_values_[kRowHumidity], text);

    char sunrise[12];
    char sunset[12];
    formatLocalTime(sunrise, sizeof(sunrise), data.sunrise_utc);
    formatLocalTime(sunset, sizeof(sunset), data.sunset_utc);
    std::snprintf(text, sizeof(text), "%s / %s", sunrise, sunset);
    lv_label_set_text(detail_values_[kRowSun], text);
}

void WeatherPlugin::renderHours(const WeatherData& data) {
    for (size_t i = 0; i < WeatherData::kMaxHours; ++i) {
        HourChip& chip = hours_[i];
        if (chip.root == nullptr) {
            return;  // body not built yet
        }

        // Hide unused chips rather than draw empty cards. An hourly strip that is short because
        // upstream sent fewer hours should look deliberate, not broken.
        if (i >= data.hour_count) {
            lv_obj_add_flag(chip.root, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(chip.root, LV_OBJ_FLAG_HIDDEN);

        const WeatherHour& hour = data.hours[i];

        char text[24];
        formatLocalTime(text, sizeof(text), hour.time_utc);
        lv_label_set_text(chip.time, text);

        std::snprintf(text, sizeof(text), "%d\xC2\xB0", wholeDegrees(hour.temperature_c));
        lv_label_set_text(chip.temperature, text);

        lv_label_set_text(chip.sky, skyDescription(hour.sky));

        if (hour.precipitation_probability >= kRainWorthShowing) {
            std::snprintf(text, sizeof(text), "%ld%% rain",
                          static_cast<long>(hour.precipitation_probability));
            lv_label_set_text(chip.rain, text);
        } else {
            // Empty rather than "0%": the absence of a rain figure is the readable way to say
            // "nothing to worry about", and it keeps the strip quiet on a clear day.
            lv_label_set_text(chip.rain, "");
        }
    }
}

}  // namespace plugins
