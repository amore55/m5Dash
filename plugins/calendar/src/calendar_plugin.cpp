#include "plugins/calendar_plugin.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace theme = dashboard::theme;
namespace timeutil = dashboard::timeutil;

constexpr const char* kTag = "calendar";
constexpr const char* kNoData = "--";

/// Fast cadence while a device code is on screen: an owner who has just finished signing in on
/// their phone should see this page catch up within seconds, not minutes. Deliberately looser
/// than Microsoft's typical 5 s requested poll interval — GraphCalendarProvider gates the actual
/// token-endpoint request against that interval internally, so ticking the plugin a little slower
/// than the request cadence costs nothing and this number is free to be a round, readable value.
constexpr uint32_t kAwaitingSignInRefreshMs = 8u * 1000u;

/// Slow cadence once signed in. Reading a work calendar every few minutes is what a person
/// glancing at a desk dashboard actually needs; every few seconds would be pointless and would
/// not be a polite way to poll a shared tenant's Graph throttling.
constexpr uint32_t kAuthorizedRefreshMs = 5u * 60u * 1000u;

lv_color_t colourForAuthState(GraphAuthState state) {
    switch (state) {
        case GraphAuthState::Authorized:
            return theme::ok();
        case GraphAuthState::AwaitingSignIn:
            return theme::stale();
        case GraphAuthState::Error:
            return theme::error();
        case GraphAuthState::NotConfigured:
        default:
            return theme::textMuted();
    }
}

}  // namespace

CalendarPlugin::CalendarPlugin() : PluginBase("calendar", "Today") {}

uint32_t CalendarPlugin::refreshIntervalMs() const {
    std::lock_guard<std::mutex> lock(modelMutex());
    return (auth_status_.state == GraphAuthState::AwaitingSignIn) ? kAwaitingSignInRefreshMs
                                                                  : kAuthorizedRefreshMs;
}

void CalendarPlugin::setAccount(const char* tenant, const char* client_id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        if (tenant != nullptr && !tenant_.equals(tenant)) {
            tenant_.assign(tenant);
            changed = true;
        }
        if (client_id != nullptr && !client_id_.equals(client_id)) {
            client_id_.assign(client_id);
            changed = true;
        }
    }
    if (changed) {
        refresh(/*force=*/true);
    }
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void CalendarPlugin::buildBody(lv_obj_t* body) {
    // Both views are children of the body and only one is visible, exactly the pattern the
    // GitHub page uses for its list/detail toggle — see that file for why this is not two
    // registered pages.
    buildAuthView(body);
    buildListView(body);
    showAuthView();
}

void CalendarPlugin::buildAuthView(lv_obj_t* parent) {
    auth_view_ = lv_obj_create(parent);
    theme::makePlain(auth_view_);
    lv_obj_set_size(auth_view_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(auth_view_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(auth_view_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(auth_view_, theme::kGapM, LV_PART_MAIN);

    auth_heading_ =
        theme::makeLabel(auth_view_, "Sign in to see your calendar", theme::fontTitle(),
                         theme::textSecondary());

    // The code itself is the thing read from across a desk, so it gets the hero treatment — a
    // person squinting at this from their chair is the entire reason device-code flow shows a
    // code at all rather than a QR code or a link only.
    auth_code_ = theme::makeLabel(auth_view_, "", theme::fontHero(), theme::textPrimary());

    auth_uri_ = theme::makeLabel(auth_view_, "", theme::fontBody(), theme::accent());
    auth_note_ = theme::makeLabel(auth_view_, "", theme::fontLabel(), theme::textMuted());
    lv_obj_set_width(auth_note_, LV_PCT(80));
    lv_label_set_long_mode(auth_note_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(auth_note_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void CalendarPlugin::buildListView(lv_obj_t* parent) {
    list_view_ = lv_obj_create(parent);
    theme::makePlain(list_view_);
    lv_obj_set_width(list_view_, LV_PCT(100));
    lv_obj_set_flex_grow(list_view_, 1);
    lv_obj_set_flex_flow(list_view_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_view_, theme::kGapS, LV_PART_MAIN);

    for (size_t i = 0; i < kMaxRows; ++i) {
        EventRow& row = rows_[i];
        row.root = theme::makeCard(list_view_);
        lv_obj_set_width(row.root, LV_PCT(100));
        lv_obj_set_height(row.root, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row.root, theme::kGapM, LV_PART_MAIN);

        // Fixed width so every row's title starts in the same column — the same reasoning as the
        // Elizabeth line board and the GitHub list, both of which line up a leading time/number
        // column for the same legibility reason.
        row.time_range = theme::makeLabel(row.root, "", theme::fontBody(), theme::textPrimary());
        lv_obj_set_width(row.time_range, 190);

        row.subject = theme::makeLabel(row.root, "", theme::fontBody(), theme::textPrimary());
        lv_obj_set_flex_grow(row.subject, 1);
        lv_label_set_long_mode(row.subject, LV_LABEL_LONG_DOT);

        row.location = theme::makeLabel(row.root, "", theme::fontLabel(), theme::textMuted());
        lv_obj_set_width(row.location, 260);
        lv_label_set_long_mode(row.location, LV_LABEL_LONG_DOT);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
    }

    list_empty_ =
        theme::makeLabel(list_view_, "No meetings today", theme::fontBody(), theme::textMuted());
    lv_obj_add_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
}

void CalendarPlugin::showAuthView() {
    if (auth_view_ != nullptr) lv_obj_remove_flag(auth_view_, LV_OBJ_FLAG_HIDDEN);
    if (list_view_ != nullptr) lv_obj_add_flag(list_view_, LV_OBJ_FLAG_HIDDEN);
}

void CalendarPlugin::showListView() {
    if (auth_view_ != nullptr) lv_obj_add_flag(auth_view_, LV_OBJ_FLAG_HIDDEN);
    if (list_view_ != nullptr) lv_obj_remove_flag(list_view_, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t CalendarPlugin::fetch(bool force) {
    (void)force;

    dashboard::MediumString tenant;
    dashboard::MediumString client_id;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        tenant = tenant_;
        client_id = client_id_;
    }

    dashboard::net::ResponseBuffer buffer(GraphCalendarProvider::kCalendarResponseBytes);
    if (!buffer.valid()) {
        setError("out of memory");
        return ESP_ERR_NO_MEM;
    }

    CalendarDay day;
    GraphAuthStatus status;
    const esp_err_t err =
        provider_.refresh(tenant.c_str(), client_id.c_str(), buffer.data(), buffer.capacity(),
                          day, status);

    {
        std::lock_guard<std::mutex> lock(modelMutex());
        auth_status_ = status;
        if (day.valid) {
            day_ = day;
        }
    }
    markDirty();

    // The state machine reports what a person would actually want to know. NotConfigured and
    // AwaitingSignIn are not failures — see clock_plugin.cpp's identical treatment of "waiting for
    // time sync" — but they are not "Ok" either, so they go through setError() with a benign
    // message rather than being silently reported as a healthy Ok with no data.
    switch (status.state) {
        case GraphAuthState::NotConfigured:
            setError("add tenant + client ID in settings");
            return ESP_ERR_INVALID_STATE;
        case GraphAuthState::AwaitingSignIn:
            setError("waiting for sign-in");
            return ESP_ERR_INVALID_STATE;
        case GraphAuthState::Error:
            setError(status.message.empty() ? "sign-in error" : status.message.c_str());
            return ESP_FAIL;
        case GraphAuthState::Authorized:
            if (!day.valid) {
                // Signed in fine; THIS cycle's calendar fetch failed. Keep whatever day_ already
                // held (done above by only overwriting when day.valid) and say so.
                setError("could not read the calendar");
                return err;
            }
            return ESP_OK;
    }
    return ESP_OK;
}

void CalendarPlugin::updateUi() {
    CalendarDay day;
    GraphAuthStatus status;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        day = day_;
        status = auth_status_;
    }

    if (status.state == GraphAuthState::Authorized) {
        showListView();
        renderList(day, timeutil::systemTimeValid() ? timeutil::nowUtc() : 0);
    } else {
        showAuthView();
        renderAuth(status);
    }
}

void CalendarPlugin::renderAuth(const GraphAuthStatus& status) {
    if (auth_heading_ == nullptr) {
        return;
    }

    switch (status.state) {
        case GraphAuthState::NotConfigured:
            lv_label_set_text(auth_heading_, "Calendar not set up");
            lv_label_set_text(auth_code_, "");
            lv_label_set_text(auth_uri_, "");
            lv_label_set_text(auth_note_,
                              "Add your Microsoft Tenant ID and Client ID on the settings page.");
            break;
        case GraphAuthState::AwaitingSignIn:
            lv_label_set_text(auth_heading_, "Sign in to see your calendar");
            lv_label_set_text(auth_code_, status.user_code.c_str());
            lv_label_set_text(auth_uri_, status.verification_uri.c_str());
            lv_label_set_text(
                auth_note_, "Open that address on your phone or computer and enter the code.");
            break;
        case GraphAuthState::Error:
            lv_label_set_text(auth_heading_, "Sign-in problem");
            lv_label_set_text(auth_code_, "");
            lv_label_set_text(auth_uri_, "");
            lv_label_set_text(auth_note_,
                              status.message.empty() ? "Trying again shortly."
                                                     : status.message.c_str());
            break;
        case GraphAuthState::Authorized:
            // Not rendered here — updateUi() shows the list view instead. Kept as a case only so
            // the switch stays exhaustive and a new enum value is a compiler error, not a silent
            // gap.
            break;
    }

    lv_obj_set_style_text_color(auth_heading_, colourForAuthState(status.state), LV_PART_MAIN);
}

void CalendarPlugin::renderList(const CalendarDay& day, std::time_t now_utc) {
    if (list_empty_ == nullptr) {
        return;
    }

    if (!day.valid || day.count == 0) {
        lv_obj_remove_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    const CalendarEvent* next = nextEvent(day, now_utc);

    for (size_t i = 0; i < kMaxRows; ++i) {
        EventRow& row = rows_[i];
        if (row.root == nullptr) {
            continue;
        }
        if (i >= day.count) {
            lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row.root, LV_OBJ_FLAG_HIDDEN);

        const CalendarEvent& event = day.events[i];
        std::tm start_local = {};
        std::tm end_local = {};
        localtime_r(&event.start_utc, &start_local);
        localtime_r(&event.end_utc, &end_local);

        char start_text[8];
        char end_text[8];
        timeutil::formatTime24h(start_text, sizeof(start_text), start_local,
                                /*with_seconds=*/false);
        timeutil::formatTime24h(end_text, sizeof(end_text), end_local, /*with_seconds=*/false);

        char range[20];
        std::snprintf(range, sizeof(range), "%s - %s", start_text, end_text);
        lv_label_set_text(row.time_range, range);

        lv_label_set_text(row.subject, event.subject.empty() ? "(no title)"
                                                             : event.subject.c_str());
        lv_label_set_text(row.location, event.location.empty() ? kNoData
                                                               : event.location.c_str());

        // The current-or-next meeting is the one worth a glance picking out — same idea as the
        // Elizabeth line board highlighting the soonest departure, applied to a card list rather
        // than a fixed-column board.
        const bool is_next = (&event == next);
        lv_obj_set_style_border_color(row.root, is_next ? theme::accent() : theme::border(),
                                      LV_PART_MAIN);
        lv_obj_set_style_border_width(row.root, is_next ? 2 : theme::kHairline, LV_PART_MAIN);
    }
}

void CalendarPlugin::summarise(dashboard::PluginSummary& out) const {
    CalendarDay day;
    GraphAuthStatus status;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        day = day_;
        status = auth_status_;
    }

    if (status.state != GraphAuthState::Authorized) {
        out.primary.assign(kNoData);
        out.secondary.assign(status.state == GraphAuthState::AwaitingSignIn
                                 ? "Sign-in needed"
                                 : "Add calendar in settings");
        out.tone = dashboard::SummaryTone::Warn;
        return;
    }

    const std::time_t now = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;
    const CalendarEvent* next = nextEvent(day, now);
    if (next == nullptr) {
        out.primary.assign("Free");
        out.secondary.assign("No more meetings today");
        out.tone = dashboard::SummaryTone::Good;
        return;
    }

    std::tm start_local = {};
    localtime_r(&next->start_utc, &start_local);
    char start_text[8];
    timeutil::formatTime24h(start_text, sizeof(start_text), start_local, /*with_seconds=*/false);
    out.primary.assign(start_text);

    // Sized for the worst case the compiler can see: two 63-character MediumStrings either side
    // of the bullet. Written into a MediumString (64) afterwards, so the tile truncates there —
    // this buffer only has to avoid the -Werror=format-truncation the smaller size triggered.
    char line[160];
    if (next->location.empty()) {
        std::snprintf(line, sizeof(line), "%s", next->subject.c_str());
    } else {
        std::snprintf(line, sizeof(line), "%s \xE2\x80\xA2 %s", next->subject.c_str(),
                      next->location.c_str());
    }
    out.secondary.assign(line);
    out.tone = dashboard::SummaryTone::Neutral;
}

}  // namespace plugins
