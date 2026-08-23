#include "plugins/elizabeth_model.hpp"

#include <cstdio>
#include <cstring>

#include "dashboard/json_util.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;

/// Suffixes that add nothing on a departure board, where every destination is a station.
constexpr const char* kDestinationSuffixes[] = {
    " Rail Station",
    " Underground Station",
    " Station",
};

/// Bounded copy with deliberate truncation.
///
/// Not snprintf("%s"): these copy a value of unknown length into a small fixed field, and
/// -Werror=format-truncation reasons about the worst case and rejects it — correctly, in the sense
/// that truncation really can happen here. Spelling the bound out says "truncating is the intent"
/// rather than silencing a warning about it.
void copyBounded(const char* source, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (source == nullptr) {
        out[0] = '\0';
        return;
    }
    const size_t length = std::strlen(source);
    const size_t copy = (length < capacity - 1) ? length : capacity - 1;
    std::memcpy(out, source, copy);
    out[copy] = '\0';
}

/// Insert `candidate` into a board that is kept sorted by time, keeping only the soonest.
///
/// The feed is NOT sorted — observed order at Liverpool Street interleaved Abbey Wood and Shenfield
/// trains arbitrarily — so taking the first five in payload order would show the wrong trains. An
/// insertion sort into a five-slot array needs no allocation and no second pass.
void insertDeparture(BoardData& board, const Departure& candidate) {
    size_t position = 0;
    while (position < board.count && board.departures[position].seconds_away <= candidate.seconds_away) {
        ++position;
    }
    if (position >= BoardData::kMaxDepartures) {
        return;  // later than everything already held, and the board is full
    }

    // Shuffle the tail down one slot, dropping whatever falls off the end.
    const size_t last =
        (board.count < BoardData::kMaxDepartures) ? board.count : BoardData::kMaxDepartures - 1;
    for (size_t i = last; i > position; --i) {
        board.departures[i] = board.departures[i - 1];
    }
    board.departures[position] = candidate;
    if (board.count < BoardData::kMaxDepartures) {
        ++board.count;
    }
}

}  // namespace

ServiceLevel serviceLevelFromSeverity(int32_t severity) {
    // TfL's scale, lowest (worst) first. Values are from the live feed's own documentation of
    // statusSeverity; anything unrecognised deliberately falls through to Unknown rather than being
    // guessed at as Good, because reporting a good service that is not running is the one error
    // that would actually strand someone.
    switch (severity) {
        case 1:   // Closed
        case 2:   // Suspended
        case 4:   // Planned Closure
        case 16:  // Not Running
        case 20:  // Service Closed
            return ServiceLevel::Closed;

        case 3:   // Part Suspended
        case 5:   // Part Closure
        case 6:   // Severe Delays
        case 11:  // Part Closed
        case 15:  // Diverted
            return ServiceLevel::Severe;

        case 0:   // Special Service
        case 7:   // Reduced Service
        case 8:   // Bus Service
        case 9:   // Minor Delays
        case 14:  // Change of frequency
        case 17:  // Issues Reported
        case 19:  // Information
            return ServiceLevel::Minor;

        case 10:  // Good Service
        case 18:  // No Issues
            return ServiceLevel::Good;

        default:
            return ServiceLevel::Unknown;
    }
}

bool parseLineStatus(const char* json_text, size_t len, LineStatus& out) {
    out = LineStatus{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    // The response is an array of lines. One line was asked for, so element 0 is it.
    const cJSON* line = json::at(doc.root(), 0);
    if (line == nullptr) {
        return false;
    }

    const cJSON* statuses = json::array(line, "lineStatuses");
    const size_t count = json::arraySize(statuses);
    if (count == 0) {
        return false;
    }

    // Worst wins. Lower severity is worse, so this is a minimum, and it starts above TfL's highest
    // real value so the first candidate always takes.
    int32_t worst = 1000;
    const cJSON* chosen = nullptr;
    for (size_t i = 0; i < count; ++i) {
        const cJSON* status = json::at(statuses, i);
        int32_t severity = 0;
        if (!json::integer(status, "statusSeverity", severity)) {
            continue;
        }
        if (severity < worst) {
            worst = severity;
            chosen = status;
        }
    }
    if (chosen == nullptr) {
        return false;
    }

    out.severity = worst;
    out.level = serviceLevelFromSeverity(worst);
    json::string(chosen, "statusSeverityDescription", out.description);
    // From the SAME status object as the severity, so a "Severe Delays" heading can never be
    // captioned with a different status's explanation.
    json::string(chosen, "reason", out.reason);
    out.valid = true;
    return true;
}

void normalisePlatform(const char* raw, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    out[0] = '\0';
    if (raw == nullptr) {
        return;
    }

    const char* text = raw;
    constexpr const char* kPrefix = "Platform ";
    const size_t prefix_len = std::strlen(kPrefix);
    if (std::strncmp(text, kPrefix, prefix_len) == 0) {
        text += prefix_len;
    }
    // "Platform Unknown" means the feed does not know, which on a board is a blank, not the word
    // "Unknown" taking up a column.
    if (std::strcmp(text, "Unknown") == 0) {
        return;
    }
    copyBounded(text, out, capacity);
}

void shortenDestination(const char* raw, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    out[0] = '\0';
    if (raw == nullptr) {
        return;
    }
    copyBounded(raw, out, capacity);

    for (const char* suffix : kDestinationSuffixes) {
        const size_t out_len = std::strlen(out);
        const size_t suffix_len = std::strlen(suffix);
        if (out_len > suffix_len && std::strcmp(out + out_len - suffix_len, suffix) == 0) {
            out[out_len - suffix_len] = '\0';
            break;
        }
    }
}

bool parseArrivals(const char* json_text, size_t len, const char* require_destination_naptan,
                   const char* exclude_destination_naptan, BoardData& out) {
    out = BoardData{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    const cJSON* predictions = doc.root();
    const size_t count = json::arraySize(predictions);

    for (size_t i = 0; i < count; ++i) {
        const cJSON* prediction = json::at(predictions, i);
        if (prediction == nullptr) {
            continue;
        }

        char destination_naptan[32] = {};
        json::string(prediction, "destinationNaptanId", destination_naptan,
                     sizeof(destination_naptan));

        if (require_destination_naptan != nullptr &&
            std::strcmp(destination_naptan, require_destination_naptan) != 0) {
            continue;
        }
        if (exclude_destination_naptan != nullptr &&
            std::strcmp(destination_naptan, exclude_destination_naptan) == 0) {
            continue;
        }

        int32_t seconds = 0;
        if (!json::integer(prediction, "timeToStation", seconds)) {
            continue;
        }
        if (seconds < 0) {
            continue;  // already gone
        }

        Departure departure;
        departure.seconds_away = seconds;
        departure.destination_naptan.assign(destination_naptan);

        char raw[96] = {};
        if (json::string(prediction, "destinationName", raw, sizeof(raw))) {
            char shortened[96];
            shortenDestination(raw, shortened, sizeof(shortened));
            departure.destination.assign(shortened);
        }

        if (json::string(prediction, "platformName", raw, sizeof(raw))) {
            char platform[32];
            normalisePlatform(raw, platform, sizeof(platform));
            departure.platform.assign(platform);
        }

        // expectedArrival is ISO-8601 with a Z, which parseIso8601Utc handles. It is only used for
        // the "22:11" column; the countdown comes from timeToStation, so a missing or malformed
        // timestamp costs the time column and nothing else.
        char when[40] = {};
        if (json::string(prediction, "expectedArrival", when, sizeof(when))) {
            std::time_t parsed = 0;
            if (dashboard::timeutil::parseIso8601Utc(when, parsed)) {
                departure.expected_utc = parsed;
            }
        }

        insertDeparture(out, departure);
    }

    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------------------------
// Per-train status
// ---------------------------------------------------------------------------------------

const char* departureStatusText(DepartureStatus status) {
    switch (status) {
        case DepartureStatus::OnTime:
            return "On time";
        case DepartureStatus::Delayed:
            return "Delayed";
        case DepartureStatus::Cancelled:
            return "Cancelled";
        case DepartureStatus::Unknown:
        default:
            // Deliberately empty rather than "Unknown": a board that cannot vouch for a train
            // should say nothing about it, not label it. See the header note on partial coverage.
            return "";
    }
}

bool parseMinutesSeconds(const char* text, int32_t& seconds) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    const char* colon = std::strchr(text, ':');
    if (colon == nullptr || colon == text || colon[1] == '\0') {
        return false;
    }

    int32_t minutes = 0;
    for (const char* p = text; p < colon; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        minutes = minutes * 10 + (*p - '0');
    }

    int32_t secs = 0;
    for (const char* p = colon + 1; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        secs = secs * 10 + (*p - '0');
    }

    seconds = minutes * 60 + secs;
    return true;
}

namespace {

DepartureStatus statusFromText(const char* text) {
    if (text == nullptr) {
        return DepartureStatus::Unknown;
    }
    // TfL spells these without a separator. Only these three were observed across four stations;
    // anything else is left Unknown rather than guessed at, because a wrong status on a departure
    // board is worse than no status.
    if (std::strcmp(text, "OnTime") == 0) {
        return DepartureStatus::OnTime;
    }
    if (std::strcmp(text, "Delayed") == 0) {
        return DepartureStatus::Delayed;
    }
    if (std::strcmp(text, "Cancelled") == 0) {
        return DepartureStatus::Cancelled;
    }
    return DepartureStatus::Unknown;
}

/// Insert a status hint, keeping the table sorted by soonest and dropping the latest on overflow.
///
/// The same lesson the board itself already learned: this feed is NOT ordered by time, so keeping
/// the first N entries keeps an arbitrary N. The board shows the soonest departures, so those are
/// the statuses that matter; a late-sorting train is the safe one to drop.
///
/// Keeping the first N was measured failing twice. A 16-slot table filled exactly on a 21-entry
/// night-time response, and after raising it to 32 the daytime response filled that exactly too.
void insertHint(StatusTable& table, const StatusHint& candidate) {
    size_t position = 0;
    while (position < table.count && table.hints[position].seconds_away <= candidate.seconds_away) {
        ++position;
    }
    if (position >= StatusTable::kMaxHints) {
        return;  // later than everything held, and the table is full
    }
    const size_t last =
        (table.count < StatusTable::kMaxHints) ? table.count : StatusTable::kMaxHints - 1;
    for (size_t i = last; i > position; --i) {
        table.hints[i] = table.hints[i - 1];
    }
    table.hints[position] = candidate;
    if (table.count < StatusTable::kMaxHints) {
        ++table.count;
    }
}

}  // namespace

bool parseDepartureStatuses(const char* json_text, size_t len, StatusTable& out) {
    out = StatusTable{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    const cJSON* entries = doc.root();
    const size_t count = json::arraySize(entries);

    // No early exit on a full table: entries arrive unsorted, so a later one may still displace a
    // held entry. insertHint() decides what survives.
    for (size_t i = 0; i < count; ++i) {
        const cJSON* entry = json::at(entries, i);
        if (entry == nullptr) {
            continue;
        }

        // The discriminator between a departure and an arrival. An entry with no time-to-departure
        // is a train terminating here, and has no place on a board of trains leaving.
        char countdown[16] = {};
        if (!json::string(entry, "minutesAndSecondsToDeparture", countdown, sizeof(countdown))) {
            continue;
        }
        int32_t seconds = 0;
        if (!parseMinutesSeconds(countdown, seconds)) {
            continue;
        }

        char status_text[24] = {};
        json::string(entry, "departureStatus", status_text, sizeof(status_text));
        const DepartureStatus status = statusFromText(status_text);
        if (status == DepartureStatus::Unknown) {
            continue;  // nothing to contribute
        }

        StatusHint hint;
        hint.seconds_away = seconds;
        hint.status = status;
        json::string(entry, "destinationNaptanId", hint.destination_naptan,
                     sizeof(hint.destination_naptan));
        insertHint(out, hint);
    }

    return true;
}

void applyDepartureStatuses(const StatusTable& table, BoardData& board) {
    for (size_t i = 0; i < board.count; ++i) {
        Departure& departure = board.departures[i];

        int32_t best_gap = kStatusMatchSeconds + 1;
        DepartureStatus best = DepartureStatus::Unknown;

        for (size_t j = 0; j < table.count; ++j) {
            const StatusHint& hint = table.hints[j];
            if (hint.destination_naptan[0] != '\0' &&
                !departure.destination_naptan.equals(hint.destination_naptan)) {
                continue;
            }
            int32_t gap = hint.seconds_away - departure.seconds_away;
            if (gap < 0) {
                gap = -gap;
            }
            if (gap < best_gap) {
                best_gap = gap;
                best = hint.status;
            }
        }

        if (best_gap <= kStatusMatchSeconds) {
            departure.status = best;
        }
    }
}

}  // namespace plugins
