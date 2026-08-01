// Time formatting, parsing and window arithmetic.
//
// Deliberately free of any ESP-IDF dependency — it uses only <ctime> and POSIX setenv/tzset —
// so test/host/src/test_time_utils.cpp compiles the very same source the firmware runs.
//
// British conventions are baked in where the brief asked for them: 24-hour clock, day-month-
// year ordering, and Europe/London handled through a POSIX TZ *rule* string rather than a
// fixed offset. That last point is what makes British Summer Time automatic without shipping
// a timezone database: see dash::cfg::kDefaultTimezone.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace dashboard::timeutil {

/// 2021-01-01T00:00:00Z. Any system time below this means the clock was never set — neither
/// SNTP nor the RTC has produced a value — so callers should treat the time as unknown rather
/// than displaying 1970.
constexpr std::time_t kMinPlausibleEpoch = 1609459200;

// ---------------------------------------------------------------------------------------
// System clock / timezone
// ---------------------------------------------------------------------------------------

/// Install a POSIX TZ string (e.g. "GMT0BST,M3.5.0/1,M10.5.0/2") and re-read it.
/// Must be called before any localtime conversion, and again whenever the setting changes.
void setTimezone(const char* posix_tz);

/// True if the system clock has been set to something plausible.
bool systemTimeValid();

std::time_t nowUtc();

/// Current local broken-down time. Zeroed if the clock is not yet valid.
std::tm localNow();

/// Local minutes since midnight, 0..1439, or -1 if the clock is not valid.
int localMinutesSinceMidnight();

// ---------------------------------------------------------------------------------------
// Pure helpers (host-tested)
// ---------------------------------------------------------------------------------------

/// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
/// `m` is 1..12, `d` is 1..31.
long long daysFromCivil(int y, unsigned m, unsigned d);

/// Broken-down UTC time -> Unix epoch seconds. Returns -1 for an out-of-range input.
///
/// Used instead of timegm(), which is a GNU extension we would rather not depend on, and
/// instead of mktime(), which would wrongly apply the local timezone.
std::time_t utcToEpoch(const std::tm& tm);

/// Parse an ISO-8601 timestamp to UTC epoch seconds.
///
/// Accepts, because these are the shapes the four upstream APIs actually emit:
///   2026-08-01T15:04:05Z          (TfL, Anthropic)
///   2026-08-01T15:04:05.123Z      (fractional seconds ignored)
///   2026-08-01T15:04:05+01:00     (offset applied)
///   2026-08-01T15:04:05+0100
///   2026-08-01T15:04               (seconds default to 0 — Open-Meteo's shape)
///   2026-08-01 15:04:05           (space instead of T)
///   2026-08-01                     (midnight)
///
/// A missing zone designator is treated as UTC. Note that Open-Meteo returns times in the
/// *requested* timezone with no designator, so the weather provider asks for
/// `timeformat=unixtime` and avoids this path entirely.
///
/// Returns false without touching `out` on anything malformed.
bool parseIso8601Utc(const char* text, std::time_t& out);

/// Parse "HH:MM" (or "H:MM") into minutes since midnight. Rejects out-of-range values.
bool parseHhMm(const char* text, int& minutes_since_midnight);

/// Render minutes-since-midnight as "HH:MM".
void formatHhMm(char* out, size_t out_len, int minutes_since_midnight);

/// True when `minute` falls in [start_min, end_min), correctly handling a window that wraps
/// midnight (e.g. a 22:30 -> 07:00 dim schedule). A window where start == end, or either
/// bound is negative, is treated as disabled and always returns false.
///
/// This is the single definition used by both the backlight day/night schedule and the TfL
/// commute periods.
bool inTimeWindow(int minute, int start_min, int end_min);

// ---------------------------------------------------------------------------------------
// Formatting. Every function is bounds-safe and always NUL-terminates.
// ---------------------------------------------------------------------------------------

/// "15:04" or "15:04:05". 24-hour, always zero-padded.
void formatTime24h(char* out, size_t out_len, const std::tm& tm, bool with_seconds);

/// "Saturday 1 August 2026". Note the day number has no leading zero, which is the British
/// long-form convention.
void formatBritishDate(char* out, size_t out_len, const std::tm& tm);

/// "01/08/2026" — day/month/year.
void formatBritishDateShort(char* out, size_t out_len, const std::tm& tm);

/// "Sat 1 Aug".
void formatBritishDateAbbrev(char* out, size_t out_len, const std::tm& tm);

/// A countdown: "now", "45s", "12m 34s", "3h 07m", "2d 04h".
/// Zero or negative renders as "now", because a reset time in the past means it has happened.
void formatCountdown(char* out, size_t out_len, long long seconds_remaining);

/// Human age of a timestamp: "never", "just now", "7 min ago", "3 h ago", "2 days ago".
/// `then <= 0` renders "never", which is how an absent cache entry reads.
void formatRelativeAge(char* out, size_t out_len, std::time_t then, std::time_t now);

}  // namespace dashboard::timeutil
