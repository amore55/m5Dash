#include "dashboard/storage/settings.hpp"

#include <algorithm>
#include <cstring>

namespace dashboard::storage {
namespace {

/// Does `list` (comma-separated, no spaces assumed but tolerated) contain `needle`?
bool csvContains(const char* list, const char* needle) {
    if (list == nullptr || needle == nullptr || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = std::strlen(needle);
    const char* p = list;
    while (*p != '\0') {
        while (*p == ' ' || *p == ',') {
            ++p;
        }
        const char* start = p;
        while (*p != '\0' && *p != ',') {
            ++p;
        }
        // Trim trailing spaces from this element.
        const char* end = p;
        while (end > start && *(end - 1) == ' ') {
            --end;
        }
        const size_t len = static_cast<size_t>(end - start);
        if (len == needle_len && std::strncmp(start, needle, len) == 0) {
            return true;
        }
    }
    return false;
}

/// Clamp minutes-since-midnight to 0..1439.
int32_t clampMinutes(int32_t value) {
    return std::clamp<int32_t>(value, 0, 1439);
}

}  // namespace

const char* toString(ClaudeProvider provider) {
    switch (provider) {
        case ClaudeProvider::Relay:
            return "relay";
        case ClaudeProvider::Direct:
            return "direct";
        case ClaudeProvider::Mock:
            return "mock";
        case ClaudeProvider::Disabled:
            break;
    }
    return "disabled";
}

ClaudeProvider claudeProviderFromString(const char* text) {
    if (text == nullptr) {
        return ClaudeProvider::Disabled;
    }
    if (std::strcmp(text, "relay") == 0) {
        return ClaudeProvider::Relay;
    }
    if (std::strcmp(text, "direct") == 0) {
        return ClaudeProvider::Direct;
    }
    if (std::strcmp(text, "mock") == 0) {
        return ClaudeProvider::Mock;
    }
    // Anything unrecognised — including a value written by newer firmware — disables the page
    // rather than guessing. A disabled page explains itself; a wrongly-guessed provider would
    // fail confusingly at runtime.
    return ClaudeProvider::Disabled;
}

void Settings::clampToValidRanges() {
    // The two brightnesses are clamped DIFFERENTLY, and the asymmetry is the point.
    //
    // Day brightness has a floor because nothing would ever raise it again: a device set to 0 all
    // day is a black screen with no schedule and no event to recover it — indistinguishable from
    // dead, reachable only by reflashing. 10 % is dim but unmistakably on.
    //
    // Night brightness may legitimately be 0 (a bedroom), and is survivable precisely because
    // touching the screen wakes it to the day level — see Backlight::wake().
    brightness_percent = std::clamp<int32_t>(brightness_percent, kMinDayBrightnessPercent, 100);
    night_brightness_percent = std::clamp<int32_t>(night_brightness_percent, 0, 100);

    dim_start_minutes = clampMinutes(dim_start_minutes);
    dim_end_minutes = clampMinutes(dim_end_minutes);

    commute_morning_start_minutes = clampMinutes(commute_morning_start_minutes);
    commute_morning_end_minutes = clampMinutes(commute_morning_end_minutes);
    commute_evening_start_minutes = clampMinutes(commute_evening_start_minutes);
    commute_evening_end_minutes = clampMinutes(commute_evening_end_minutes);

    // A timeout longer than a day is indistinguishable from "never" and would be a confusing
    // thing to display, so it collapses to never (0).
    if (lock_idle_timeout_minutes < 0 || lock_idle_timeout_minutes > 24 * 60) {
        lock_idle_timeout_minutes = 0;
    }

    // Out-of-range coordinates would send a nonsense request to Open-Meteo and produce a
    // confusing API error rather than an obvious configuration problem.
    if (latitude < -90.0 || latitude > 90.0) {
        latitude = 51.5072;
    }
    if (longitude < -180.0 || longitude > 180.0) {
        longitude = -0.1276;
    }

    if (!clock_style.equals("minimal") && !clock_style.equals("flap")) {
        clock_style.assign("minimal");
    }
    if (!ota_channel.equals("stable") && !ota_channel.equals("dev")) {
        ota_channel.assign("stable");
    }
    if (timezone.empty()) {
        timezone.assign(dash::cfg::kDefaultTimezone);
    }
}

bool Settings::pageEnabled(const char* plugin_id) const {
    // Empty list means "everything", which is the sane default for a fresh device and also the
    // safe interpretation of a cleared value.
    if (enabled_pages.empty()) {
        return true;
    }
    return csvContains(enabled_pages.c_str(), plugin_id);
}

}  // namespace dashboard::storage
