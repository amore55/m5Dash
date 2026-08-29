// Every user-configurable value on the device, in one place.
//
// TWO DELIBERATE PROPERTIES:
//
// 1. **No secrets live here.** Wi-Fi passwords, the Telegram bot token, the Claude credential,
//    the TfL key and the lock PIN hash are in SecretStore, in a separate NVS namespace. That
//    separation is what lets this struct be logged, dumped to the setup portal, or serialised
//    for debugging without any risk of leaking a credential — and lets NVS encryption, when
//    enabled, protect exactly the partition that needs it.
//
// 2. **No heap.** Every string is a FixedString, so the memory cost of the whole configuration
//    is known at compile time and a long value truncates rather than allocating.
//
// Schema versioning: `schema` is persisted alongside the values. Adding a field is free —
// it simply reads back as its default on an older device. Renaming or changing the MEANING of a
// field requires a migration step; see settings_migrate.hpp.

#pragma once

#include <cstdint>

#include "app_config.hpp"
#include "dashboard/fixed_string.hpp"

namespace dashboard::storage {

/// Which Claude usage provider the plugin should use.
enum class ClaudeProvider : uint8_t {
    Disabled,  ///< Page shows as disabled. The default: nothing is configured out of the box.
    Relay,     ///< Fetch from a user-supplied HTTPS endpoint returning the documented schema.
    Direct,    ///< Talk to Anthropic directly. Unverified — see docs/CLAUDE_USAGE.md.
    Mock,      ///< Clearly-labelled fake data, for UI work without credentials.
};

const char* toString(ClaudeProvider provider);
ClaudeProvider claudeProviderFromString(const char* text);

/// Floor under the DAYTIME brightness. See Settings::clampToValidRanges() for why night has no
/// such floor and this does.
constexpr int32_t kMinDayBrightnessPercent = 10;

struct Settings {
    /// Bump ONLY when an existing field changes meaning or name. Adding fields does not need it.
    static constexpr uint32_t kCurrentSchema = 3;

    uint32_t schema = kCurrentSchema;

    // ---- device ------------------------------------------------------------------------
    MediumString device_name{"desk-dashboard"};
    /// POSIX TZ rule string, not an IANA name — this is what makes BST automatic with no
    /// timezone database on the device. See dash::cfg::kDefaultTimezone.
    LongString timezone{dash::cfg::kDefaultTimezone};

    // ---- network (password lives in SecretStore) ---------------------------------------
    MediumString wifi_ssid;

    // ---- display -----------------------------------------------------------------------
    int32_t brightness_percent = 70;
    int32_t night_brightness_percent = 12;
    /// Dim window in local minutes since midnight. start == end disables the schedule.
    /// Wrapping past midnight is normal and handled by timeutil::inTimeWindow().
    int32_t dim_start_minutes = 22 * 60 + 30;
    int32_t dim_end_minutes = 7 * 60;

    ShortString clock_style{"minimal"};  ///< "minimal" | "flap"
    bool show_seconds = false;

    /// Turn the landscape image 180 degrees, for mounting the device the other way round.
    /// Still landscape either way — this is not a portrait option.
    bool display_flipped = false;

    // ---- idle lock screen --------------------------------------------------------------
    // PIN itself is stored salted+hashed in SecretStore. If lock_enabled is true but no PIN has
    // been set, the device deliberately does NOT lock — a half-finished settings change must
    // never lock the user out of the only interface the device has.
    bool lock_enabled = false;
    int32_t lock_idle_timeout_minutes = 15;  ///< 0 = never lock
    bool lock_show_weather = false;
    ShortString wallpaper_style{"clock"};

    // ---- weather -----------------------------------------------------------------------
    MediumString weather_label{"London"};
    /// Stored, not geocoded per refresh — the brief is explicit about that.
    double latitude = 51.5072;
    double longitude = -0.1276;

    // ---- Elizabeth line ----------------------------------------------------------------
    int32_t commute_morning_start_minutes = 7 * 60;
    int32_t commute_morning_end_minutes = 9 * 60 + 30;
    int32_t commute_evening_start_minutes = 16 * 60 + 30;
    int32_t commute_evening_end_minutes = 19 * 60;

    // ---- Telegram (token lives in SecretStore) -----------------------------------------
    /// Messages from any other sender are ignored. 0 means "not configured".
    int64_t telegram_allowed_user_id = 0;

    // ---- Calendar (Microsoft Graph; the refresh token lives in SecretStore) -------------
    /// Entra ID tenant: the GUID, or a verified domain like "contoso.onmicrosoft.com". NOT a
    /// secret — it identifies the organisation, the same way a GitHub org name does, and appears
    /// in every request URL regardless.
    ///
    /// Deliberately not defaulted to "common" or "organizations": those endpoints only work for a
    /// MULTI-tenant app registration, and a single-tenant one — the ordinary, simpler choice, and
    /// the one this device's setup instructions assume — is only reachable through its own
    /// tenant's endpoint.
    MediumString ms_tenant_id;

    /// The Application (client) ID from the Entra ID app registration. Not a secret: device-code
    /// sign-in is a PUBLIC client flow by design and needs no client secret at all — the whole
    /// exchange happens by the owner typing a code into a browser on another device.
    MediumString ms_client_id;

    // ---- Claude (credential lives in SecretStore) --------------------------------------
    ClaudeProvider claude_provider = ClaudeProvider::Disabled;
    MediumString claude_organisation_id;
    UrlString claude_relay_url;

    // ---- GitHub ------------------------------------------------------------------------
    /// Whose repositories the GitHub page shows, and which of them count as "mine" for the
    /// filter. Not a secret — it appears in a URL. The token is in SecretStore.
    ShortString github_username{"amore55"};

    /// Which of the two lists the page is showing: work repositories, or the user's own.
    /// Toggled from the page itself, and persisted so the choice survives a restart.
    ///
    /// Replaced an earlier `github_all_repositories`, whose NVS key ("gh_all") is now unused and
    /// left behind deliberately — reusing a key whose MEANING changed is exactly what schema
    /// migrations exist to prevent, and abandoning one costs nothing.
    bool github_show_work = false;

    /// Organisation whose repositories the WORK token covers. Optional.
    ///
    /// When set, work repositories are fetched from /orgs/{this}/repos, which is the reliable
    /// route for a fine-grained token owned by that organisation. When empty the page falls back
    /// to /user/repos?affiliation=organization_member, which depends on the token being able to
    /// enumerate the user's organisations and may therefore come back empty.
    ShortString github_organisation;

    /// Friendly names for GitHub logins, as `login=Name` pairs separated by commas.
    ///
    /// A setting rather than a table in the firmware, so a new colleague is a text edit rather
    /// than a reflash. An unlisted login is shown as itself, never blank.
    FixedString<192> github_aliases{"colgateteeth200=Yusuf,morfry=Morgan,amore55=Moreno"};

    // ---- pages -------------------------------------------------------------------------
    ShortString default_page{"summary"};
    /// Comma-separated plugin ids. Empty means "all". Unknown ids are ignored on load, so a
    /// saved list from older firmware cannot hide a page that has since been added.
    FixedString<160> enabled_pages;
    /// Raised from 160 when the seventh page landed: the schema-2 migration prepends "summary,"
    /// and appends ",github" to whatever was stored, and 160 could not hold the result.
    ///
    /// "claude" dropped from the default (2026-08-24): the page is retired for now — see
    /// app_main.cpp. A device with "claude" already in its STORED order keeps it there
    /// harmlessly; PageManager ignores an id with no registered plugin, so no migration was
    /// needed to remove one, unlike renaming "todos" to "calendar" was.
    FixedString<224> page_order{"summary,clock,weather,elizabeth,calendar,github"};

    // ---- OTA ---------------------------------------------------------------------------
    ShortString ota_channel{"stable"};
    UrlString ota_manifest_url;
    bool ota_automatic_install = false;

    // ---- helpers -----------------------------------------------------------------------

    /// True once enough is configured for the device to leave first-run setup.
    /// Only Wi-Fi matters: every integration degrades gracefully without its own settings.
    bool provisioned() const { return !wifi_ssid.empty(); }

    /// Clamp every numeric field into a sane range. Called after loading, so a corrupted or
    /// hand-edited NVS value cannot put the UI into an impossible state.
    void clampToValidRanges();

    /// Is `plugin_id` listed in enabled_pages? An empty list means everything is enabled.
    bool pageEnabled(const char* plugin_id) const;
};

}  // namespace dashboard::storage
