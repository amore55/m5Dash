// Microsoft Graph: sign-in and the calendar fetch, behind one interface.
//
// SIGN-IN IS THE OAUTH 2.0 DEVICE AUTHORIZATION GRANT ("device code flow"), and it is the right
// choice for exactly the reason it exists: this device has no keyboard worth typing a password on
// and no browser at all. The flow instead has the DEVICE show a short code and a URL; the owner
// reads them off the screen and completes sign-in on a phone or laptop that already has both. No
// credential of the owner's ever reaches this firmware — only an opaque, revocable OAuth token
// does, and it carries only the one permission (Calendars.Read) that was granted to it.
//
// THE CALENDAR NEVER SEES A CLIENT SECRET, deliberately: device-code flow is a PUBLIC client
// flow, which is what makes it possible to embed the client ID in firmware at all. A client
// secret is a password for the app itself, and a firmware image is not a place a password can be
// kept — it is dumped to every device in the fleet and readable by anyone who reflashes one.
//
// WHAT ENTRA ID REQUIRES OF THE APP REGISTRATION (documented here because it lives outside this
// repo and is easy to get half-right): "Allow public client flows" = Yes, and the delegated
// Graph permission `Calendars.Read` + `offline_access`, consented. `Calendars.ReadBasic` is NOT
// enough — it deliberately excludes location, which is the room this page exists to show.
//
// REFRESH TOKENS ROTATE. Microsoft issues a new refresh token on every use for a public client,
// which is why ensureAccessToken() stores whatever comes back on EVERY successful call, not only
// the first. Keeping the old one around after a rotation is not a fallback, it is a token that no
// longer works.

#pragma once

#include <cstddef>
#include <ctime>

#include "esp_err.h"

#include "dashboard/fixed_string.hpp"
#include "dashboard/net/https_client.hpp"
#include "plugins/calendar_model.hpp"

namespace plugins {

enum class GraphAuthState : uint8_t {
    /// No tenant / client ID set in Settings. Nothing has been attempted.
    NotConfigured,

    /// A device code has been requested and is on screen; waiting for the owner to complete
    /// sign-in elsewhere.
    AwaitingSignIn,

    /// A usable access token was obtained (fresh, or via a stored refresh token). The calendar
    /// fetch that follows may still fail on its own — this only describes the SIGN-IN.
    Authorized,

    /// Sign-in itself failed in a way that is not "waiting" — declined, expired with nobody
    /// completing it, or a stored refresh token was rejected. Recoverable: the next cycle starts
    /// a fresh device code automatically.
    Error,
};

struct GraphAuthStatus {
    GraphAuthState state = GraphAuthState::NotConfigured;

    /// Set only while AwaitingSignIn. Read out and typed by a human, so kept short and plain
    /// rather than percent-decoded or otherwise machine-shaped.
    dashboard::MediumString user_code;
    dashboard::UrlString verification_uri;

    /// Human-readable detail for the Error state. Never a token or any part of one.
    dashboard::MediumString message;
};

class GraphCalendarProvider {
  public:
    /// OAuth responses (device code / token) are small, fixed-shape JSON — a few hundred bytes in
    /// practice. Generous headroom costs nothing since it comes from PSRAM regardless.
    static constexpr size_t kAuthResponseBytes = 4 * 1024;

    /// UNMEASURED against a live mailbox — see calendar_model.hpp's file header for why. Sized
    /// well above the no-Prefer-header, $select-trimmed response this provider requests for
    /// kMaxEvents worth of ordinary meetings.
    static constexpr size_t kCalendarResponseBytes = 24 * 1024;

    /// Drive sign-in exactly one step further, then — only if that step leaves us Authorized —
    /// fetch today's events into `day`.
    ///
    /// Returns ESP_OK whenever this cycle completed without a TRANSPORT failure, whatever the
    /// resulting `status.state` is: AwaitingSignIn and Error are both normal, expected outcomes of
    /// an OAuth exchange that came back and answered, not failures of this call. `day` is only
    /// touched when `status.state` ends this call as Authorized and the calendar fetch itself
    /// also succeeded; check `day.valid` to tell the two apart from a plain reading of the state.
    ///
    /// `tenant` / `client_id` empty ⇒ status.state = NotConfigured and nothing is requested at
    /// all — not even a wasted attempt that would only report the obvious.
    esp_err_t refresh(const char* tenant, const char* client_id, char* buffer, size_t capacity,
                      CalendarDay& day, GraphAuthStatus& status);

  private:
    esp_err_t beginDeviceCodeFlow(const char* tenant, const char* client_id, char* buffer,
                                  size_t capacity, GraphAuthStatus& status);
    esp_err_t pollDeviceCodeToken(const char* tenant, const char* client_id, char* buffer,
                                  size_t capacity, GraphAuthStatus& status,
                                  char* out_access_token, size_t access_token_capacity);
    esp_err_t refreshAccessToken(const char* tenant, const char* client_id, char* buffer,
                                 size_t capacity, GraphAuthStatus& status,
                                 char* out_access_token, size_t access_token_capacity);
    esp_err_t fetchToday(const char* access_token, char* buffer, size_t capacity, CalendarDay& day,
                        GraphAuthStatus& status);

    dashboard::net::HttpsClient http_;

    // ---- device-code-in-progress state ---------------------------------------------------
    // In-memory only, on purpose: a device code is worthless after ~15 minutes and irrelevant
    // across a reboot, so none of this belongs in SecretStore or Settings.
    //
    // LongString, not the shorter MediumString user_code/verification_uri get: Entra ID does not
    // document a fixed length for this one and it is never read by a person, so there is no
    // reason to risk it being the field that truncates.
    dashboard::LongString device_code_;
    int poll_interval_s_ = 5;
    std::time_t device_code_expires_utc_ = 0;

    /// What is currently on screen. Re-issued into `status` on every call that keeps the SAME
    /// code showing (too soon to poll again, or Microsoft said "still waiting") — without this,
    /// those branches would have nothing to put in status.user_code and the screen would blank
    /// between polls.
    dashboard::MediumString pending_user_code_;
    dashboard::UrlString pending_verification_uri_;

    /// Not-before time for the NEXT token-endpoint poll, so a plugin refresh cadence faster than
    /// Microsoft's requested interval (or one stretched by a "slow_down" response) cannot spam the
    /// endpoint into rate-limiting the sign-in it is trying to complete.
    std::time_t next_poll_utc_ = 0;
};

}  // namespace plugins
