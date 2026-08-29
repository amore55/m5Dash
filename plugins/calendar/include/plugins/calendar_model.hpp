// Today's calendar, from Microsoft Graph, and the parser that produces it.
//
// ESP-free and LVGL-free, like every other *_model.hpp in this project, so it can be exercised
// against a saved fixture without ESP-IDF or LVGL.
//
// WHAT GRAPH'S calendarView RETURNS — decided the shape of this file, though none of it has yet
// been measured against a LIVE mailbox. There is no test tenant to query from here, so the sizes
// below are a considered starting point, not a measurement — see the note on kMaxEvents.
//
//  1. calendarView, not /events. /events lists the RECURRENCE MASTER for a repeating meeting, not
//     its individual occurrences — a daily standup would appear once, dated whenever the series
//     started, with no way to tell it also happens today. calendarView expands recurrence into
//     concrete instances between the two dates given, which is what "today's meetings" means.
//
//  2. Every dateTime arrives WITHOUT a 'Z' or offset — "2026-08-24T09:00:00.0000000" — with the
//     zone carried separately in a sibling "timeZone" field. No Prefer header is sent, so that
//     field is always "UTC", and dashboard::timeutil::parseIso8601Utc already treats a bare
//     timestamp with no designator as UTC — so the shared parser needs no changes for this API.
//
//  3. isAllDay and isCancelled must both be checked. An all-day entry (a holiday, an OOO banner)
//     is not a meeting to stand a room next to, and one calendarView still lists as cancelled is
//     not really on the day any more either.
//
//  4. The room lives at location.displayName, free text the organiser typed — there is no
//     structured "room" field to rely on. It is often long ("Meeting Room 3 - Building A, Floor
//     2") and is truncated for display rather than the board being built around it.

#pragma once

#include <cstddef>
#include <ctime>

#include "dashboard/fixed_string.hpp"

namespace plugins {

struct CalendarEvent {
    dashboard::MediumString subject;

    /// location.displayName. Empty when the organiser set no location.
    dashboard::MediumString location;

    std::time_t start_utc = 0;
    std::time_t end_utc = 0;
};

struct CalendarDay {
    /// UNMEASURED against a live mailbox — see the file header. Twelve is a guess at "a
    /// genuinely busy day", sized to revisit once real data exists, the same caution the TfL and
    /// GitHub integrations learned the hard way: a cap this project has reached exactly before
    /// was never a coincidence.
    static constexpr size_t kMaxEvents = 12;

    CalendarEvent events[kMaxEvents];
    size_t count = 0;

    /// True once a response has been parsed, even with zero events — a genuinely free day is a
    /// real state and must read differently from "never fetched".
    bool valid = false;
};

/// Parse `GET /me/calendarview?...`. The response is `{"value": [ ...events... ]}`.
///
/// Entries are kept in the order the API returned them. The request this is paired with
/// (calendar_provider.cpp) asks for `$orderby=start/dateTime`, so this does NOT re-sort — unlike
/// the TfL and GitHub feeds, which distrust feed order for reasons specific to those APIs.
bool parseCalendarView(const char* json, size_t len, CalendarDay& out);

/// The soonest event in `day` that has not yet ENDED — so a meeting already in progress counts,
/// not only one still to come. Used for both the summary tile and the highlighted row: if you are
/// currently in a meeting, that is the meeting worth showing, not the one after it.
///
/// Returns nullptr if `day` is invalid or every event has already finished.
const CalendarEvent* nextEvent(const CalendarDay& day, std::time_t now_utc);

}  // namespace plugins
