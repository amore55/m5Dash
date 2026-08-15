#include "plugins/tfl_provider.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "dashboard/storage/secret_store.hpp"

namespace plugins {
namespace {

constexpr const char* kTag = "elizabeth";

constexpr size_t kUrlBytes = 512;

using dashboard::storage::Secret;
using dashboard::storage::SecretStore;

/// Turn a transport or status failure into something worth putting in a footer.
const char* describeFailure(const dashboard::net::HttpResponse& response) {
    if (response.truncated) {
        return "service data too large";
    }
    if (response.status == 429) {
        return "TfL is rate limiting us";
    }
    if (response.status >= 500) {
        return "TfL service unavailable";
    }
    if (response.status >= 400) {
        return "TfL refused the request";
    }
    return "could not reach TfL";
}

}  // namespace

const char* journeyOrigin(Journey journey) {
    return journey == Journey::ToLiverpoolStreet ? "Abbey Wood" : "Liverpool Street";
}

const char* journeyDestination(Journey journey) {
    return journey == Journey::ToLiverpoolStreet ? "Liverpool Street" : "Abbey Wood";
}

bool TflProvider::appendAppKey(char* url, size_t capacity) const {
    if (!SecretStore::has(Secret::TflAppKey)) {
        return true;  // unauthenticated is the normal, supported case
    }

    char key[dashboard::storage::kMaxSecretLength] = {};
    if (SecretStore::get(Secret::TflAppKey, key, sizeof(key)) != ESP_OK) {
        return true;
    }

    const size_t used = std::strlen(url);
    // '?' or '&' depending on whether this URL already carries a query.
    const char separator = (std::strchr(url, '?') != nullptr) ? '&' : '?';
    const int written =
        std::snprintf(url + used, capacity - used, "%capp_key=%s", separator, key);
    // Wipe the local copy before the frame goes away. The URL itself still holds it, which is
    // unavoidable, and is exactly why HttpsClient truncates every log line at the '?'.
    std::memset(key, 0, sizeof(key));

    return written > 0 && static_cast<size_t>(written) < capacity - used;
}

esp_err_t TflProvider::fetchStatus(char* buffer, size_t capacity, LineStatus& out) {
    char url[kUrlBytes];
    std::snprintf(url, sizeof(url), "https://api.tfl.gov.uk/Line/elizabeth/Status");
    if (!appendAppKey(url, sizeof(url))) {
        last_error_ = "internal error";
        return ESP_ERR_INVALID_SIZE;
    }

    dashboard::net::HttpRequest request;
    request.url = url;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);
    if (err != ESP_OK) {
        last_error_ = describeFailure(response);
        return err;
    }

    if (!parseLineStatus(buffer, response.length, out)) {
        last_error_ = "status response was not understood";
        ESP_LOGW(kTag, "status parse failed on %u bytes", static_cast<unsigned>(response.length));
        return ESP_ERR_INVALID_RESPONSE;
    }

    last_error_ = "";
    ESP_LOGI(kTag, "status: %s (severity %ld)", out.description.c_str(),
             static_cast<long>(out.severity));
    return ESP_OK;
}

esp_err_t TflProvider::fetchBoard(Journey journey, char* buffer, size_t capacity, BoardData& out) {
    // Direction is a SERVER-side filter and the only reliable one: the `direction` field inside the
    // predictions is frequently blank. It also roughly halves the response.
    //
    // At Abbey Wood every departure is outbound (it is a terminus, there is one way out); at
    // Liverpool Street the Abbey Wood branch is inbound.
    const char* origin = (journey == Journey::ToLiverpoolStreet) ? stops::kAbbeyWood
                                                                 : stops::kLiverpoolStreet;
    const char* direction =
        (journey == Journey::ToLiverpoolStreet) ? "outbound" : "inbound";

    char url[kUrlBytes];
    std::snprintf(url, sizeof(url),
                  "https://api.tfl.gov.uk/Line/elizabeth/Arrivals/%s?direction=%s", origin,
                  direction);
    if (!appendAppKey(url, sizeof(url))) {
        last_error_ = "internal error";
        return ESP_ERR_INVALID_SIZE;
    }

    dashboard::net::HttpRequest request;
    request.url = url;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);
    if (err != ESP_OK) {
        last_error_ = describeFailure(response);
        return err;
    }

    // Heading INTO town, the destination varies (Paddington, Reading, Heathrow, Maidenhead) and all
    // of them call at Liverpool Street, so nothing is required — only Abbey Wood itself is excluded,
    // to keep terminating arrivals off a departure board.
    //
    // Heading home, inbound covers both the Abbey Wood and Shenfield branches, so Abbey Wood is
    // required.
    const char* require = (journey == Journey::ToAbbeyWood) ? stops::kAbbeyWood : nullptr;
    const char* exclude = (journey == Journey::ToLiverpoolStreet) ? stops::kAbbeyWood : nullptr;

    if (!parseArrivals(buffer, response.length, require, exclude, out)) {
        last_error_ = "arrivals response was not understood";
        ESP_LOGW(kTag, "arrivals parse failed on %u bytes",
                 static_cast<unsigned>(response.length));
        return ESP_ERR_INVALID_RESPONSE;
    }

    last_error_ = "";
    ESP_LOGI(kTag, "board %s to %s: %u departures from %u bytes", journeyOrigin(journey),
             journeyDestination(journey), static_cast<unsigned>(out.count),
             static_cast<unsigned>(response.length));
    return ESP_OK;
}

esp_err_t TflProvider::fetchDepartureStatuses(Journey journey, char* buffer, size_t capacity,
                                              StatusTable& out) {
    const char* origin = (journey == Journey::ToLiverpoolStreet) ? stops::kAbbeyWood
                                                                 : stops::kLiverpoolStreet;

    // No direction parameter on this endpoint — it is StopPoint-scoped, not Line-scoped. The
    // departure/arrival split is done by the parser instead, on whether an entry has a
    // time-to-departure at all.
    char url[kUrlBytes];
    std::snprintf(url, sizeof(url),
                  "https://api.tfl.gov.uk/StopPoint/%s/ArrivalDepartures?lineIds=elizabeth",
                  origin);
    if (!appendAppKey(url, sizeof(url))) {
        last_error_ = "internal error";
        return ESP_ERR_INVALID_SIZE;
    }

    dashboard::net::HttpRequest request;
    request.url = url;
    // One attempt, not the usual three. This is enrichment: if it fails the board is still correct,
    // just without status, and spending the full retry schedule would delay a refresh that has
    // already succeeded.
    request.max_attempts = 1;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);
    if (err != ESP_OK) {
        // Deliberately does NOT set last_error_: the caller keeps its board, and putting this in
        // the footer would report a page as degraded when the thing a person reads off it is fine.
        ESP_LOGW(kTag, "departure status unavailable: %s", esp_err_to_name(err));
        return err;
    }

    if (!parseDepartureStatuses(buffer, response.length, out)) {
        ESP_LOGW(kTag, "departure status parse failed on %u bytes",
                 static_cast<unsigned>(response.length));
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(kTag, "departure status: %u entries from %u bytes", static_cast<unsigned>(out.count),
             static_cast<unsigned>(response.length));
    return ESP_OK;
}

}  // namespace plugins
