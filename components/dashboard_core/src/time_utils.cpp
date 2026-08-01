#include "dashboard/time_utils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dashboard::timeutil {
namespace {

constexpr const char* kDayNames[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                      "Thursday", "Friday", "Saturday"};
constexpr const char* kDayAbbrev[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kMonthNames[12] = {"January",   "February", "March",    "April",
                                         "May",       "June",     "July",     "August",
                                         "September", "October",  "November", "December"};
constexpr const char* kMonthAbbrev[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

constexpr long long kSecondsPerMinute = 60;
constexpr long long kSecondsPerHour = 3600;
constexpr long long kSecondsPerDay = 86400;

/// Read exactly `count` digits. Returns false if fewer are present, so "2026-8-01" is
/// rejected rather than silently mis-parsed.
bool readFixedDigits(const char*& p, int count, int& out) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        value = value * 10 + (p[i] - '0');
    }
    p += count;
    out = value;
    return true;
}

bool isLeap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int daysInMonth(int year, int month_1_based) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month_1_based < 1 || month_1_based > 12) {
        return 0;
    }
    if (month_1_based == 2 && isLeap(year)) {
        return 29;
    }
    return kDays[month_1_based - 1];
}

void safeCopy(char* out, size_t out_len, const char* text) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "%s", text);
}

}  // namespace

// ---------------------------------------------------------------------------------------
// System clock
// ---------------------------------------------------------------------------------------

void setTimezone(const char* posix_tz) {
    if (posix_tz == nullptr || posix_tz[0] == '\0') {
        return;
    }
    setenv("TZ", posix_tz, 1);
    tzset();
}

bool systemTimeValid() { return std::time(nullptr) >= kMinPlausibleEpoch; }

std::time_t nowUtc() { return std::time(nullptr); }

std::tm localNow() {
    std::tm out = {};
    const std::time_t now = std::time(nullptr);
    if (now < kMinPlausibleEpoch) {
        return out;  // all-zero; callers check systemTimeValid() before trusting this
    }
    localtime_r(&now, &out);
    return out;
}

int localMinutesSinceMidnight() {
    if (!systemTimeValid()) {
        return -1;
    }
    const std::tm tm = localNow();
    return tm.tm_hour * 60 + tm.tm_min;
}

// ---------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------

long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);                // [0, 399]
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;  // [0, 365]
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;            // [0, 146096]
    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

std::time_t utcToEpoch(const std::tm& tm) {
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 ||
        tm.tm_hour < 0 || tm.tm_hour > 23 || tm.tm_min < 0 || tm.tm_min > 59 ||
        tm.tm_sec < 0 || tm.tm_sec > 60) {
        return static_cast<std::time_t>(-1);
    }
    const long long days = daysFromCivil(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1),
                                         static_cast<unsigned>(tm.tm_mday));
    return static_cast<std::time_t>(days * kSecondsPerDay + tm.tm_hour * kSecondsPerHour +
                                    tm.tm_min * kSecondsPerMinute + tm.tm_sec);
}

bool parseIso8601Utc(const char* text, std::time_t& out) {
    if (text == nullptr) {
        return false;
    }
    const char* p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (!readFixedDigits(p, 4, year) || *p++ != '-') {
        return false;
    }
    if (!readFixedDigits(p, 2, month) || *p++ != '-') {
        return false;
    }
    if (!readFixedDigits(p, 2, day)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) {
        return false;
    }

    int hour = 0;
    int minute = 0;
    int second = 0;
    long long offset_seconds = 0;

    if (*p == 'T' || *p == 't' || *p == ' ') {
        ++p;
        if (!readFixedDigits(p, 2, hour) || *p++ != ':') {
            return false;
        }
        if (!readFixedDigits(p, 2, minute)) {
            return false;
        }
        if (*p == ':') {
            ++p;
            if (!readFixedDigits(p, 2, second)) {
                return false;
            }
        }
        // Fractional seconds: accepted and discarded. Sub-second precision is meaningless
        // for a dashboard and carrying it would only complicate the arithmetic.
        if (*p == '.' || *p == ',') {
            ++p;
            if (*p < '0' || *p > '9') {
                return false;  // a separator with no digits after it is malformed
            }
            while (*p >= '0' && *p <= '9') {
                ++p;
            }
        }

        if (*p == 'Z' || *p == 'z') {
            ++p;
        } else if (*p == '+' || *p == '-') {
            const int sign = (*p == '-') ? -1 : 1;
            ++p;
            int off_hour = 0;
            int off_min = 0;
            if (!readFixedDigits(p, 2, off_hour)) {
                return false;
            }
            if (*p == ':') {
                ++p;
            }
            if (*p >= '0' && *p <= '9') {
                if (!readFixedDigits(p, 2, off_min)) {
                    return false;
                }
            }
            if (off_hour > 23 || off_min > 59) {
                return false;
            }
            // The offset is what must be SUBTRACTED from local time to reach UTC.
            offset_seconds =
                sign * (off_hour * kSecondsPerHour + off_min * kSecondsPerMinute);
        }
        // No designator at all: treated as UTC.
    }

    if (hour > 23 || minute > 59 || second > 60) {
        return false;
    }

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (*p != '\0') {
        return false;  // trailing junk — better to reject than to guess
    }

    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;

    const std::time_t local = utcToEpoch(tm);
    if (local == static_cast<std::time_t>(-1)) {
        return false;
    }
    out = static_cast<std::time_t>(local - offset_seconds);
    return true;
}

bool parseHhMm(const char* text, int& minutes_since_midnight) {
    if (text == nullptr) {
        return false;
    }
    const char* p = text;
    while (*p == ' ') {
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    int hour = *p++ - '0';
    if (*p >= '0' && *p <= '9') {
        hour = hour * 10 + (*p++ - '0');
    }
    if (*p++ != ':') {
        return false;
    }
    int minute = 0;
    if (!readFixedDigits(p, 2, minute)) {
        return false;
    }
    while (*p == ' ') {
        ++p;
    }
    if (*p != '\0' || hour > 23 || minute > 59) {
        return false;
    }
    minutes_since_midnight = hour * 60 + minute;
    return true;
}

void formatHhMm(char* out, size_t out_len, int minutes_since_midnight) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (minutes_since_midnight < 0 || minutes_since_midnight > 1439) {
        safeCopy(out, out_len, "--:--");
        return;
    }
    std::snprintf(out, out_len, "%02d:%02d", minutes_since_midnight / 60,
                  minutes_since_midnight % 60);
}

bool inTimeWindow(int minute, int start_min, int end_min) {
    if (start_min < 0 || end_min < 0 || start_min == end_min) {
        return false;  // disabled
    }
    if (minute < 0) {
        return false;  // unknown time cannot be inside a window
    }
    if (start_min < end_min) {
        return minute >= start_min && minute < end_min;
    }
    // Wraps midnight, e.g. 22:30 -> 07:00.
    return minute >= start_min || minute < end_min;
}

// ---------------------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------------------

void formatTime24h(char* out, size_t out_len, const std::tm& tm, bool with_seconds) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (with_seconds) {
        std::snprintf(out, out_len, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        std::snprintf(out, out_len, "%02d:%02d", tm.tm_hour, tm.tm_min);
    }
}

void formatBritishDate(char* out, size_t out_len, const std::tm& tm) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    // Bounds-check before indexing the name tables: a std::tm from a failed conversion can
    // hold anything, and an out-of-range read here would be a crash rather than a wrong date.
    const int wday = (tm.tm_wday >= 0 && tm.tm_wday < 7) ? tm.tm_wday : 0;
    const int mon = (tm.tm_mon >= 0 && tm.tm_mon < 12) ? tm.tm_mon : 0;
    std::snprintf(out, out_len, "%s %d %s %d", kDayNames[wday], tm.tm_mday, kMonthNames[mon],
                  tm.tm_year + 1900);
}

void formatBritishDateShort(char* out, size_t out_len, const std::tm& tm) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "%02d/%02d/%04d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
}

void formatBritishDateAbbrev(char* out, size_t out_len, const std::tm& tm) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    const int wday = (tm.tm_wday >= 0 && tm.tm_wday < 7) ? tm.tm_wday : 0;
    const int mon = (tm.tm_mon >= 0 && tm.tm_mon < 12) ? tm.tm_mon : 0;
    std::snprintf(out, out_len, "%s %d %s", kDayAbbrev[wday], tm.tm_mday, kMonthAbbrev[mon]);
}

void formatCountdown(char* out, size_t out_len, long long seconds_remaining) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (seconds_remaining <= 0) {
        safeCopy(out, out_len, "now");
        return;
    }
    if (seconds_remaining < kSecondsPerMinute) {
        std::snprintf(out, out_len, "%llds", seconds_remaining);
        return;
    }
    if (seconds_remaining < kSecondsPerHour) {
        std::snprintf(out, out_len, "%lldm %02llds", seconds_remaining / kSecondsPerMinute,
                      seconds_remaining % kSecondsPerMinute);
        return;
    }
    if (seconds_remaining < kSecondsPerDay) {
        std::snprintf(out, out_len, "%lldh %02lldm", seconds_remaining / kSecondsPerHour,
                      (seconds_remaining % kSecondsPerHour) / kSecondsPerMinute);
        return;
    }
    std::snprintf(out, out_len, "%lldd %02lldh", seconds_remaining / kSecondsPerDay,
                  (seconds_remaining % kSecondsPerDay) / kSecondsPerHour);
}

void formatRelativeAge(char* out, size_t out_len, std::time_t then, std::time_t now) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (then <= 0) {
        safeCopy(out, out_len, "never");
        return;
    }
    long long delta = static_cast<long long>(now) - static_cast<long long>(then);
    if (delta < 0) {
        // Clock went backwards (an SNTP correction, or the RTC being ahead). Reporting "just
        // now" is honest enough and avoids rendering a negative age.
        delta = 0;
    }
    if (delta < 45) {
        safeCopy(out, out_len, "just now");
        return;
    }
    if (delta < 90 * kSecondsPerMinute) {
        const long long minutes = (delta + 30) / kSecondsPerMinute;
        std::snprintf(out, out_len, "%lld min ago", minutes < 1 ? 1 : minutes);
        return;
    }
    if (delta < 36 * kSecondsPerHour) {
        std::snprintf(out, out_len, "%lld h ago", (delta + 1800) / kSecondsPerHour);
        return;
    }
    const long long days = (delta + kSecondsPerDay / 2) / kSecondsPerDay;
    std::snprintf(out, out_len, "%lld day%s ago", days, days == 1 ? "" : "s");
}

}  // namespace dashboard::timeutil
