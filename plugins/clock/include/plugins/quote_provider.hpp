// The quote under the clock, from api-ninjas.
//
// A KEY IS NOT OPTIONAL. https://api.api-ninjas.com/v2/quotes answers
//
//     {"error": "Missing API Key."}
//
// with HTTP 400 and no body worth parsing when the X-Api-Key header is absent. There is no
// anonymous tier to degrade to, so with no key stored the feature is simply switched off and the
// label is hidden — not shown empty, and not shown as an error, because a missing optional key is
// a configuration choice rather than a fault.
//
// The key goes in a HEADER, not a query parameter, which is the reason HttpsClient's
// header_name/header_value pair exists. It is never logged, and since it is a credential the
// request will not follow redirects — see the note in https_client.cpp.
//
// TWICE A DAY, AND WHY THE BUCKET IS COMPUTED RATHER THAN TIMED
//
// "A different quote twice a day" is implemented as a bucket number derived from the local date
// and whether it is before or after noon, NOT as a 12-hour timer. A timer would drift with every
// reboot and hand out a new quote each time the device restarted, which on a desk dashboard
// being developed against is several times an hour. A bucket means the same quote for the whole
// morning however many times the device restarts, and a new one at noon.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "esp_err.h"

#include "dashboard/fixed_string.hpp"
#include "dashboard/net/https_client.hpp"

namespace plugins {

struct Quote {
    bool valid = false;

    /// Bounded rather than rejected if long: a truncated quote still reads, and the page gives it
    /// two lines. 240 covers everything api-ninjas has returned in testing with room over.
    dashboard::FixedString<240> text;
    dashboard::MediumString author;

    /// Which half-day this quote belongs to. See quoteBucket().
    int64_t bucket = -1;
};

/// Which half-day `local` falls in: one number per morning and one per afternoon, increasing.
///
/// Returns -1 when the clock is not set, which callers must treat as "do not fetch yet" — a
/// bucket derived from an unsynchronised clock would be replaced minutes later and waste the
/// request.
int64_t quoteBucket(std::time_t now_utc, bool time_valid);

/// Parse an api-ninjas quotes response.
///
/// ACCEPTS BOTH SHAPES on purpose: v1 returned a one-element ARRAY of objects, and v2's docs are
/// not explicit about whether it kept that or returns a bare OBJECT. Handling both costs four
/// lines and removes a class of failure that would otherwise only show up on the device, where
/// the key lives and the response can actually be seen.
bool parseQuote(const char* json, size_t len, Quote& out);

class QuoteProvider {
  public:
    /// Enough for a handful of quotes even if the endpoint returns a list.
    static constexpr size_t kResponseBytes = 4096;

    /// True when a key is stored. False means the feature is off, not broken.
    static bool configured();

    /// Fetch one quote. Worker thread. Returns ESP_ERR_INVALID_STATE when no key is stored.
    esp_err_t fetch(char* buffer, size_t capacity, Quote& out);

    const char* lastError() const { return last_error_; }

  private:
    dashboard::net::HttpsClient http_;
    const char* last_error_ = "";
};

}  // namespace plugins
