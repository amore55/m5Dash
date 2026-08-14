// Elizabeth line service status and departure boards, and the parsers that produce them.
//
// ESP-free and LVGL-free, like weather_model.hpp, so test/host can compile it unchanged. The two
// parsers take the current time as an argument for the same reason: given the same bytes and the
// same instant they must produce the same board.
//
// WHAT THE TfL UNIFIED API ACTUALLY RETURNS — established by querying it, not from the docs. Every
// one of these mattered, and several would have produced a plausible, wrong board:
//
//  1. **A line can have MORE THAN ONE lineStatus at once.** Observed live: severity 9 "Minor
//     Delays" AND severity 6 "Severe Delays" in the same response, describing different sections.
//     Taking the first would have reported minor delays during severe ones, so the parser keeps the
//     WORST. Note that in TfL's scheme a LOWER statusSeverity is worse: 10 is Good Service, 6 is
//     Severe Delays, 2 is Suspended.
//
//  2. **`direction` is frequently an empty string** in the prediction payload, so it cannot be used
//     to filter client-side. The `direction=` QUERY PARAMETER is applied server-side and does work,
//     which is what the provider uses — and it halves the response, from ~31 KB to ~15 KB at
//     Liverpool Street.
//
//  3. **Liverpool Street has two stop points on this line.** `910GLIVST` is the mainline surface
//     station and `910GLIVSTLL` is the low-level core platforms. Abbey Wood trains run through the
//     core. Both currently return similar predictions, but `910GLIVSTLL` is the correct one and
//     gives cleaner direction values.
//
//  4. **A departure board must exclude trains TERMINATING where you are standing.** Abbey Wood's
//     unfiltered arrivals include inbound trains whose destination is Abbey Wood itself. Filtering
//     is by `destinationNaptanId` rather than by name, because names arrive in inconsistent forms
//     ("Paddington", but "Maidenhead Rail Station").
//
//  5. **Not every westbound destination is a fixed string.** Leaving Abbey Wood, trains are bound
//     for Paddington, Maidenhead, Heathrow, Reading or Hayes & Harlington. They all call at
//     Liverpool Street — Abbey Wood is a terminus and there is only one way out — so the rule for
//     that board is "everything outbound", not "everything to Liverpool Street".

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "dashboard/fixed_string.hpp"

namespace plugins {

// ---------------------------------------------------------------------------------------
// Service status
// ---------------------------------------------------------------------------------------

/// How much the status should worry the reader. Collapsed from TfL's ~20 statusSeverity values
/// because the page needs a colour and a decision, not a taxonomy.
enum class ServiceLevel : uint8_t {
    Unknown,
    Good,     ///< Good Service, No Issues.
    Minor,    ///< Minor delays, reduced service, information notices.
    Severe,   ///< Severe delays, diversions, part suspensions and part closures.
    Closed,   ///< Suspended, closed, not running.
};

/// Map a TfL `statusSeverity` onto a ServiceLevel. Remember: lower is worse.
ServiceLevel serviceLevelFromSeverity(int32_t severity);

struct LineStatus {
    bool valid = false;

    /// TfL's own severity number, kept so that "worst wins" can be computed and so an unmapped
    /// future value is still visible in the log.
    int32_t severity = -1;

    /// TfL's own words — "Good Service", "Severe Delays". Taken from the API rather than generated
    /// from the enum, so the page says exactly what the official feed says.
    dashboard::MediumString description;

    /// The disruption text. Empty on a good service.
    ///
    /// These run long: an observed one was 600 characters, including a list of every operator
    /// accepting tickets. Sized to hold the substantive part of a realistic message; anything
    /// beyond is truncated by FixedString rather than rejected.
    dashboard::FixedString<512> reason;

    ServiceLevel level = ServiceLevel::Unknown;

    /// Is there something the reader should act on? Drives whether the reason is shown at all.
    bool warning() const { return level != ServiceLevel::Good && level != ServiceLevel::Unknown; }
};

/// Parse `GET /Line/elizabeth/Status`. The response is an ARRAY of lines; element 0 is the line.
///
/// Keeps the worst of however many lineStatuses are present, and takes the reason from that same
/// status so the words and the severity cannot disagree.
bool parseLineStatus(const char* json, size_t len, LineStatus& out);

// ---------------------------------------------------------------------------------------
// Departure board
// ---------------------------------------------------------------------------------------

struct Departure {
    /// When it is expected, UTC. Rendered as the local "22:11" on the board.
    std::time_t expected_utc = 0;

    /// TfL's `timeToStation`, in seconds. This is the countdown the board shows, and it comes
    /// straight from the feed rather than being derived from expected_utc minus now — the feed's
    /// own number is what the official boards display.
    int32_t seconds_away = 0;

    dashboard::MediumString destination;

    /// "4", "A", or empty when the feed says "Platform Unknown". Normalised — see
    /// normalisePlatform().
    dashboard::ShortString platform;
};

struct BoardData {
    /// Five, because that is what was asked for and what fits at a readable size.
    static constexpr size_t kMaxDepartures = 5;

    Departure departures[kMaxDepartures];
    size_t count = 0;

    /// True once a response has been parsed, even if it yielded no departures — which is a real
    /// state (last train gone) and must be distinguishable from "never fetched".
    bool valid = false;
};

/// Strip TfL's "Platform " prefix and turn "Platform Unknown" into an empty string.
/// Exposed for host tests; the parser applies it already.
void normalisePlatform(const char* raw, char* out, size_t capacity);

/// Trim the noise from a destination name for display: "Maidenhead Rail Station" reads as
/// "Maidenhead" on a board, because every entry on it is a rail station.
void shortenDestination(const char* raw, char* out, size_t capacity);

/// Parse `GET /Line/elizabeth/Arrivals/{naptan}` into the next few departures.
///
/// Filtering, applied in this order:
///   * `require_destination_naptan`, when non-null, keeps ONLY trains bound for that stop. Used at
///     Liverpool Street, where inbound covers both the Abbey Wood and Shenfield branches.
///   * `exclude_destination_naptan`, when non-null, drops trains terminating there. Used at Abbey
///     Wood to remove arrivals from a departure board.
///
/// Entries with a negative `timeToStation` (already gone) are skipped. The result holds the five
/// soonest, in ascending order, regardless of the order the feed sent them in — which is not
/// sorted.
bool parseArrivals(const char* json, size_t len, const char* require_destination_naptan,
                   const char* exclude_destination_naptan, BoardData& out);

}  // namespace plugins
