// The TfL Unified API, behind an interface, so the page holds no HTTP and no JSON.
//
// Two calls per refresh: the line status, and one departure board. They are separate endpoints and
// either can fail on its own — a page that showed nothing because the status call timed out, while
// holding five perfectly good departures, would be worse than one that shows what it has.
//
// NO KEY IS REQUIRED. TfL serves this unauthenticated at low rates, and a 2-minute refresh of two
// endpoints is nowhere near the limit. An app key is supported because the owner may have one, and
// it is passed as a QUERY PARAMETER — which is precisely why HttpsClient never logs a query string.

#pragma once

#include <cstddef>

#include "esp_err.h"

#include "dashboard/net/https_client.hpp"
#include "plugins/elizabeth_model.hpp"

namespace plugins {

/// The two ends of this particular commute.
///
/// Naptan identifiers rather than names, because they are what the API filters on and they do not
/// drift. See elizabeth_model.hpp note 3 on why Liverpool Street is the LL variant.
namespace stops {

/// Abbey Wood, Elizabeth line platforms.
constexpr const char* kAbbeyWood = "910GABWDXR";

/// Liverpool Street LOW LEVEL — the Elizabeth line core platforms, which is where Abbey Wood
/// trains run. NOT 910GLIVST, which is the mainline surface station.
constexpr const char* kLiverpoolStreet = "910GLIVSTLL";

}  // namespace stops

/// Which way the reader is travelling. The page picks one by time of day.
enum class Journey : uint8_t {
    ToLiverpoolStreet,  ///< The morning commute: board at Abbey Wood.
    ToAbbeyWood,        ///< The evening commute: board at Liverpool Street.
};

const char* journeyOrigin(Journey journey);
const char* journeyDestination(Journey journey);

class TflProvider {
  public:
    /// Ceiling on a response.
    ///
    /// SIZE THIS FROM A DAYTIME SAMPLE, NOT A LATE-NIGHT ONE. This was 24 KB, taken at 22:10 when
    /// Liverpool Street's inbound arrivals were 15 KB across 17 predictions. The device then hit the
    /// ceiling in normal use:
    ///
    ///     W https: .../Arrivals/910GLIVSTLL: response truncated at 24576 bytes
    ///
    /// The same call at 13:53 the next day returned 28,261 bytes across 32 predictions - the evening
    /// timetable is roughly half the size of the daytime one, so the original figure was measured
    /// against the thinnest service of the day. A truncated body is deliberately not retried (see
    /// worthRetrying), so the effect was a board that passed its tests and was empty every afternoon.
    ///
    /// Current figures: status ~3.7 KB; Abbey Wood outbound ~14 KB; Liverpool Street inbound
    /// ~28 KB; ArrivalDepartures ~9 KB. The direction filter is still not optional - unfiltered was
    /// already 31 KB at night. 48 KB is ~1.7x the worst daytime observation, leaving room for a
    /// weekday peak - busier than the Sunday this was measured on - and for a disrupted timetable,
    /// which produces MORE predictions rather than fewer. The component ceiling is
    /// dash::cfg::kHttpMaxResponseBytes (64 KB), so there is headroom above this.
    ///
    /// This never becomes a member array — see ResponseBuffer. The caller passes storage in.
    static constexpr size_t kResponseBytes = 48 * 1024;

    /// `GET /Line/elizabeth/Status`. Worker thread.
    esp_err_t fetchStatus(char* buffer, size_t capacity, LineStatus& out);

    /// `GET /Line/elizabeth/Arrivals/{origin}?direction=...`. Worker thread.
    ///
    /// The direction and the destination filtering are both derived from `journey` here rather than
    /// by the caller, because getting either wrong produces a board that looks right and is wrong.
    esp_err_t fetchBoard(Journey journey, char* buffer, size_t capacity, BoardData& out);

    /// `GET /StopPoint/{origin}/ArrivalDepartures?lineIds=elizabeth`. Worker thread.
    ///
    /// A third request per refresh, for one reason: this is the only TfL endpoint that says whether
    /// an individual train is delayed or cancelled. It cannot supply the board itself — see the long
    /// note above parseDepartureStatuses(). Callers should treat a failure here as cosmetic and keep
    /// the board they already have.
    esp_err_t fetchDepartureStatuses(Journey journey, char* buffer, size_t capacity,
                                     StatusTable& out);

    /// Short, user-facing reason for the last failure. Never contains a URL or a key.
    const char* lastError() const { return last_error_; }

  private:
    /// Append the app key, if the owner has stored one. Returns false only if the URL would not fit.
    bool appendAppKey(char* url, size_t capacity) const;

    dashboard::net::HttpsClient http_;
    const char* last_error_ = "";
};

}  // namespace plugins
