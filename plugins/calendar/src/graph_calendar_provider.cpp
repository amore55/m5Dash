#include "plugins/graph_calendar_provider.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"

#include "dashboard/json_util.hpp"
#include "dashboard/storage/secret_store.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;
namespace timeutil = dashboard::timeutil;
using dashboard::storage::Secret;
using dashboard::storage::SecretStore;

constexpr const char* kTag = "graph";
constexpr const char* kGraphHost = "https://graph.microsoft.com/v1.0";

/// Both a scope big enough to read location (Calendars.Read, not the room-blind ReadBasic — see
/// the file header) and offline_access, which is what makes a refresh token exist at all.
constexpr const char* kScope = "offline_access%20Calendars.Read";

constexpr size_t kUrlBytes = 256;

/// Access tokens are JWTs and Azure AD's is not fixed-length — group-membership claims in
/// particular can inflate it well past what a short-lived string needs to be. Sized generously
/// and UNMEASURED against a live token; see calendar_model.hpp's file header for the project's
/// standing rule about sizing from a guess versus a measurement.
constexpr size_t kAccessTokenBytes = 4096;

/// Read one string field directly into a caller buffer, tolerating absence — every accessor in
/// this file is optional-by-default because an OAuth error response and a success response share
/// almost no fields.
bool readField(const cJSON* root, const char* key, char* out, size_t capacity) {
    return json::string(root, key, out, capacity);
}

/// Build the one thing this whole file writes into a query string or POST body: a straight
/// key=value join. Every value passed through here is either a fixed literal (kScope) or an
/// opaque token issued BY Microsoft to be sent back to Microsoft — a client ID GUID, a device
/// code, a refresh token — and all three are documented as URL-safe base64/GUID alphabets, so
/// there is no general percent-encoder here. If that ever stops being true for a new value, it
/// will fail loudly as an OAuth error, not silently.
void buildPostBody(char* out, size_t capacity, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(out, capacity, fmt, args);
    va_end(args);
}

/// Format a UTC instant as "YYYY-MM-DDTHH:MM:SSZ", which is what calendarView's startDateTime and
/// endDateTime query parameters expect.
///
/// gmtime_r, not dashboard::timeutil — every existing conversion in that component goes the OTHER
/// way (civil date to epoch, avoiding timegm/mktime because they apply an unwanted local
/// timezone). This is a plain epoch-to-UTC-calendar breakdown, which is exactly what gmtime_r is
/// for and introduces no local-timezone ambiguity to avoid.
void formatCalendarBound(std::time_t when, char* out, size_t capacity) {
    std::tm utc = {};
    gmtime_r(&when, &utc);
    std::snprintf(out, capacity, "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900,
                 utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
}

}  // namespace

esp_err_t GraphCalendarProvider::refresh(const char* tenant, const char* client_id, char* buffer,
                                         size_t capacity, CalendarDay& day,
                                         GraphAuthStatus& status) {
    status = GraphAuthStatus{};
    day = CalendarDay{};

    if (tenant == nullptr || tenant[0] == '\0' || client_id == nullptr || client_id[0] == '\0') {
        // Nothing to do, and nothing requested — a plugin polling every few seconds while
        // unconfigured must not turn into a background loop hitting a URL it cannot build.
        status.state = GraphAuthState::NotConfigured;
        return ESP_OK;
    }

    char access_token[kAccessTokenBytes] = {};

    if (SecretStore::has(Secret::MicrosoftRefreshToken)) {
        const esp_err_t err =
            refreshAccessToken(tenant, client_id, buffer, capacity, status, access_token,
                              sizeof(access_token));
        if (err != ESP_OK) {
            return err;  // transport failure; status already describes it
        }
        if (status.state != GraphAuthState::Authorized) {
            // The stored refresh token was rejected (invalid_grant) — refreshAccessToken() has
            // already cleared it. Fall through to start a device-code flow in this SAME call
            // rather than waiting a whole cycle, so a revoked session shows a fresh sign-in code
            // promptly instead of a bare error for a while first.
        } else {
            return fetchToday(access_token, buffer, capacity, day, status);
        }
    }

    // No refresh token (or it was just rejected above): drive the device-code flow one step.
    const std::time_t now = timeutil::nowUtc();
    if (device_code_.empty() || now >= device_code_expires_utc_) {
        return beginDeviceCodeFlow(tenant, client_id, buffer, capacity, status);
    }
    if (now < next_poll_utc_) {
        // Too soon to ask again — keep showing the same code without spending a request.
        status.state = GraphAuthState::AwaitingSignIn;
        status.user_code = pending_user_code_;
        status.verification_uri = pending_verification_uri_;
        return ESP_OK;
    }

    const esp_err_t err = pollDeviceCodeToken(tenant, client_id, buffer, capacity, status,
                                              access_token, sizeof(access_token));
    if (err != ESP_OK) {
        return err;
    }
    if (status.state == GraphAuthState::Authorized) {
        return fetchToday(access_token, buffer, capacity, day, status);
    }
    return ESP_OK;
}

esp_err_t GraphCalendarProvider::beginDeviceCodeFlow(const char* tenant, const char* client_id,
                                                     char* buffer, size_t capacity,
                                                     GraphAuthStatus& status) {
    char url[kUrlBytes];
    std::snprintf(url, sizeof(url), "https://login.microsoftonline.com/%s/oauth2/v2.0/devicecode",
                 tenant);

    char body[512];
    buildPostBody(body, sizeof(body), "client_id=%s&scope=%s", client_id, kScope);

    dashboard::net::HttpRequest request;
    request.url = url;
    request.post_body = body;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);

    if (response.truncated || response.length == 0) {
        ESP_LOGW(kTag, "device code request failed: %s", esp_err_to_name(err));
        status.state = GraphAuthState::Error;
        status.message.assign("could not start sign-in");
        return err;
    }

    json::Doc doc;
    if (!doc.parse(buffer, response.length)) {
        status.state = GraphAuthState::Error;
        status.message.assign("sign-in response not understood");
        return ESP_ERR_INVALID_RESPONSE;
    }

    char device_code[dashboard::LongString::capacity()] = {};
    char user_code[dashboard::MediumString::capacity()] = {};
    char verification_uri[dashboard::UrlString::capacity()] = {};
    int32_t expires_in = 0;
    int32_t interval = 5;

    if (!readField(doc.root(), "device_code", device_code, sizeof(device_code)) ||
        !readField(doc.root(), "user_code", user_code, sizeof(user_code)) ||
        !readField(doc.root(), "verification_uri", verification_uri, sizeof(verification_uri))) {
        // Some tenants answer "verification_uri_complete" instead/as well; the plain
        // "verification_uri" + typed user_code is the pair every Entra ID tenant supports, so no
        // fallback field is read here.
        ESP_LOGW(kTag, "device code response missing a required field");
        status.state = GraphAuthState::Error;
        status.message.assign("sign-in response incomplete");
        return ESP_ERR_INVALID_RESPONSE;
    }
    json::integer(doc.root(), "expires_in", expires_in);
    json::integer(doc.root(), "interval", interval);

    device_code_.assign(device_code);
    poll_interval_s_ = (interval > 0) ? interval : 5;
    const std::time_t now = timeutil::nowUtc();
    device_code_expires_utc_ = now + (expires_in > 0 ? expires_in : 900);
    next_poll_utc_ = now;  // first poll is allowed immediately

    status.state = GraphAuthState::AwaitingSignIn;
    status.user_code.assign(user_code);
    status.verification_uri.assign(verification_uri);
    pending_user_code_ = status.user_code;
    pending_verification_uri_ = status.verification_uri;

    ESP_LOGI(kTag, "device code issued, expires in %ld s, poll every %ld s",
             static_cast<long>(expires_in), static_cast<long>(poll_interval_s_));
    return ESP_OK;
}

esp_err_t GraphCalendarProvider::pollDeviceCodeToken(const char* tenant, const char* client_id,
                                                     char* buffer, size_t capacity,
                                                     GraphAuthStatus& status,
                                                     char* out_access_token,
                                                     size_t access_token_capacity) {
    char url[kUrlBytes];
    std::snprintf(url, sizeof(url), "https://login.microsoftonline.com/%s/oauth2/v2.0/token",
                 tenant);

    char body[768];
    buildPostBody(body, sizeof(body),
                 "grant_type=urn:ietf:params:oauth:grant-type:device_code"
                 "&client_id=%s&device_code=%s",
                 client_id, device_code_.c_str());

    dashboard::net::HttpRequest request;
    request.url = url;
    request.post_body = body;

    dashboard::net::HttpResponse response;
    const esp_err_t transport_err = http_.get(request, buffer, capacity, response);

    if (response.truncated || response.length == 0) {
        // A genuine network failure. Keep the device code and try again next cycle — the code is
        // still valid, this cycle just could not reach Microsoft to ask about it.
        ESP_LOGW(kTag, "token poll failed: %s", esp_err_to_name(transport_err));
        status.state = GraphAuthState::AwaitingSignIn;
        status.user_code = pending_user_code_;
        status.verification_uri = pending_verification_uri_;
        return ESP_OK;
    }

    json::Doc doc;
    if (!doc.parse(buffer, response.length)) {
        status.state = GraphAuthState::Error;
        status.message.assign("sign-in response not understood");
        device_code_.clear();
        return ESP_ERR_INVALID_RESPONSE;
    }

    char access_token[kAccessTokenBytes] = {};
    if (readField(doc.root(), "access_token", access_token, sizeof(access_token))) {
        char refresh_token[dashboard::storage::kMaxLongSecretLength + 1] = {};
        if (readField(doc.root(), "refresh_token", refresh_token, sizeof(refresh_token))) {
            SecretStore::set(Secret::MicrosoftRefreshToken, refresh_token);
        }
        std::memset(refresh_token, 0, sizeof(refresh_token));

        std::snprintf(out_access_token, access_token_capacity, "%s", access_token);
        std::memset(access_token, 0, sizeof(access_token));

        device_code_.clear();
        status.state = GraphAuthState::Authorized;
        ESP_LOGI(kTag, "sign-in complete");
        return ESP_OK;
    }

    char error[64] = {};
    readField(doc.root(), "error", error, sizeof(error));

    if (std::strcmp(error, "authorization_pending") == 0) {
        next_poll_utc_ = timeutil::nowUtc() + poll_interval_s_;
        status.state = GraphAuthState::AwaitingSignIn;
        status.user_code = pending_user_code_;
        status.verification_uri = pending_verification_uri_;
        return ESP_OK;
    }
    if (std::strcmp(error, "slow_down") == 0) {
        // Microsoft's own guidance is to add 5 s and keep going, not to restart the flow.
        poll_interval_s_ += 5;
        next_poll_utc_ = timeutil::nowUtc() + poll_interval_s_;
        status.state = GraphAuthState::AwaitingSignIn;
        status.user_code = pending_user_code_;
        status.verification_uri = pending_verification_uri_;
        ESP_LOGI(kTag, "Microsoft asked us to slow down; polling every %d s now",
                 poll_interval_s_);
        return ESP_OK;
    }

    // expired_token, authorization_declined, bad_verification_code, or anything unrecognised: the
    // code on screen is no longer any use. Start a fresh flow within THIS call rather than
    // reporting a bare error for a whole cycle first — the owner sees a new code appear, not a
    // dead end.
    ESP_LOGW(kTag, "device code flow ended (%s); starting a new one", error[0] ? error : "?");
    device_code_.clear();
    return beginDeviceCodeFlow(tenant, client_id, buffer, capacity, status);
}

esp_err_t GraphCalendarProvider::refreshAccessToken(const char* tenant, const char* client_id,
                                                    char* buffer, size_t capacity,
                                                    GraphAuthStatus& status,
                                                    char* out_access_token,
                                                    size_t access_token_capacity) {
    char stored_refresh[dashboard::storage::kMaxLongSecretLength + 1] = {};
    if (SecretStore::get(Secret::MicrosoftRefreshToken, stored_refresh, sizeof(stored_refresh)) !=
        ESP_OK) {
        status.state = GraphAuthState::NotConfigured;
        return ESP_OK;
    }

    char url[kUrlBytes];
    std::snprintf(url, sizeof(url), "https://login.microsoftonline.com/%s/oauth2/v2.0/token",
                 tenant);

    // Sized for a refresh token up to kMaxLongSecretLength inside the body alongside the fixed
    // parts; comfortably covers the rest with room to spare.
    char body[dashboard::storage::kMaxLongSecretLength + 256];
    buildPostBody(body, sizeof(body), "grant_type=refresh_token&client_id=%s&refresh_token=%s&scope=%s",
                 client_id, stored_refresh, kScope);
    std::memset(stored_refresh, 0, sizeof(stored_refresh));

    dashboard::net::HttpRequest request;
    request.url = url;
    request.post_body = body;

    dashboard::net::HttpResponse response;
    const esp_err_t transport_err = http_.get(request, buffer, capacity, response);
    std::memset(body, 0, sizeof(body));

    if (response.truncated || response.length == 0) {
        ESP_LOGW(kTag, "token refresh failed: %s", esp_err_to_name(transport_err));
        status.state = GraphAuthState::Error;
        status.message.assign("could not reach sign-in service");
        return transport_err;
    }

    json::Doc doc;
    if (!doc.parse(buffer, response.length)) {
        status.state = GraphAuthState::Error;
        status.message.assign("sign-in response not understood");
        return ESP_ERR_INVALID_RESPONSE;
    }

    char access_token[kAccessTokenBytes] = {};
    if (readField(doc.root(), "access_token", access_token, sizeof(access_token))) {
        char refresh_token[dashboard::storage::kMaxLongSecretLength + 1] = {};
        // ALWAYS re-store: Microsoft rotates this on every use for a public client, and the value
        // we just spent is no longer valid — see the file header.
        if (readField(doc.root(), "refresh_token", refresh_token, sizeof(refresh_token))) {
            SecretStore::set(Secret::MicrosoftRefreshToken, refresh_token);
        }
        std::memset(refresh_token, 0, sizeof(refresh_token));

        std::snprintf(out_access_token, access_token_capacity, "%s", access_token);
        std::memset(access_token, 0, sizeof(access_token));

        status.state = GraphAuthState::Authorized;
        return ESP_OK;
    }

    char error[64] = {};
    readField(doc.root(), "error", error, sizeof(error));
    ESP_LOGW(kTag, "stored refresh token rejected (%s); clearing it", error[0] ? error : "?");
    SecretStore::set(Secret::MicrosoftRefreshToken, nullptr);  // erases
    status.state = GraphAuthState::Error;
    status.message.assign("sign-in expired; a new code will appear shortly");
    return ESP_OK;
}

esp_err_t GraphCalendarProvider::fetchToday(const char* access_token, char* buffer,
                                            size_t capacity, CalendarDay& day,
                                            GraphAuthStatus& status) {
    const std::time_t now = timeutil::nowUtc();
    // A generous local day rather than exact local midnight: the device's own local-time
    // conversion already exists in timeutil, but calendarView's start/end window only needs to be
    // WIDE ENOUGH to contain today — a few hours either side of local midnight cannot pull in an
    // extra day's meetings because parseOneEvent still keeps every event calendarView returns
    // inside that window, and 36 hours either side of "now" always contains all of today
    // regardless of which timezone the device is in relative to UTC.
    // 64, not the 21 bytes "YYYY-MM-DDTHH:MM:SSZ\0" actually needs: -Werror=format-truncation
    // reasons about the worst case for a plain `int` argument to %04d/%02d (up to 11 digits with
    // a sign), not about what gmtime_r's fields are actually bounded to, and GCC's interprocedural
    // analysis sees both call sites' buffer size when checking formatCalendarBound()'s own
    // snprintf. Real runtime output is always ~20 characters.
    char start_text[64];
    char end_text[64];
    formatCalendarBound(now - 36 * 3600, start_text, sizeof(start_text));
    formatCalendarBound(now + 36 * 3600, end_text, sizeof(end_text));

    char url[384];
    std::snprintf(url, sizeof(url),
                 "%s/me/calendarview?startDateTime=%s&endDateTime=%s"
                 "&$orderby=start/dateTime&$top=%u"
                 "&$select=subject,start,end,location,isAllDay,isCancelled",
                 kGraphHost, start_text, end_text,
                 static_cast<unsigned>(CalendarDay::kMaxEvents * 2));

    dashboard::net::HttpRequest request;
    request.url = url;
    request.bearer = access_token;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "calendar fetch failed: HTTP %d", response.status);
        status.message.assign("could not read the calendar");
        // Sign-in itself is still good — leave status.state as Authorized rather than
        // downgrading it, so the page keeps its authorized chrome and just shows stale data.
        return err;
    }

    if (!parseCalendarView(buffer, response.length, day)) {
        status.message.assign("calendar response not understood");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(kTag, "%u events for today", static_cast<unsigned>(day.count));
    return ESP_OK;
}

}  // namespace plugins
