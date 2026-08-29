#include "plugins/calendar_model.hpp"

#include "dashboard/json_util.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;
namespace timeutil = dashboard::timeutil;

/// Returns false for anything that should not appear on the board at all — an all-day entry, a
/// cancelled one, or a malformed one — rather than a "valid but empty" event, so the caller can
/// simply skip and move on.
bool parseOneEvent(const cJSON* node, CalendarEvent& out) {
    if (node == nullptr) {
        return false;
    }

    bool all_day = false;
    bool cancelled = false;
    json::boolean(node, "isAllDay", all_day);
    json::boolean(node, "isCancelled", cancelled);
    if (all_day || cancelled) {
        return false;
    }

    const cJSON* start = json::object(node, "start");
    const cJSON* end = json::object(node, "end");
    if (start == nullptr || end == nullptr) {
        return false;
    }

    char start_text[40];
    char end_text[40];
    if (!json::string(start, "dateTime", start_text, sizeof(start_text)) ||
        !json::string(end, "dateTime", end_text, sizeof(end_text))) {
        return false;
    }
    if (!timeutil::parseIso8601Utc(start_text, out.start_utc) ||
        !timeutil::parseIso8601Utc(end_text, out.end_utc)) {
        return false;
    }

    json::string(node, "subject", out.subject);

    out.location.clear();
    const cJSON* location = json::object(node, "location");
    if (location != nullptr) {
        json::string(location, "displayName", out.location);
    }
    return true;
}

}  // namespace

bool parseCalendarView(const char* json_text, size_t len, CalendarDay& out) {
    out = CalendarDay{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    const cJSON* items = json::array(doc.root(), "value");
    const size_t total = json::arraySize(items);
    for (size_t i = 0; i < total && out.count < CalendarDay::kMaxEvents; ++i) {
        CalendarEvent event;
        if (parseOneEvent(json::at(items, i), event)) {
            out.events[out.count++] = event;
        }
    }

    // Set even when count == 0 and even when `total` exceeded what fit: a genuinely free day and
    // a busy one that hit the cap are both real, parsed responses, not failures.
    out.valid = true;
    return true;
}

const CalendarEvent* nextEvent(const CalendarDay& day, std::time_t now_utc) {
    if (!day.valid) {
        return nullptr;
    }
    for (size_t i = 0; i < day.count; ++i) {
        if (day.events[i].end_utc > now_utc) {
            return &day.events[i];
        }
    }
    return nullptr;
}

}  // namespace plugins
