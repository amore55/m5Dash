// Page: Today — the day's Outlook/Entra ID calendar, replacing the old to-do placeholder.
//
// TWO VIEWS IN ONE PAGE, exactly as the GitHub page does it and for the same reason: a second
// registered page would appear in the swipe rotation and in Settings as something to enable,
// which the sign-in screen is not.
//
//   * Not authorized: a big user code and a verification URL, refreshed automatically if the
//     code expires before anyone gets to it. Nothing to type on the device itself — that is the
//     whole point of the device-code flow, see graph_calendar_provider.hpp.
//   * Authorized: today's meetings, soonest first, each with its time, its title and its room.
//
// The refresh cadence is DATA-DRIVEN, the same pattern the GitHub page uses for "poll fast while
// a run is in progress": fast while AwaitingSignIn, so a code that just appeared does not sit on
// screen for minutes past when the owner has actually finished signing in on their phone; slow
// once Authorized, because polling Graph's calendar every few seconds forever would be both
// pointless and impolite to a shared work tenant's throttling.

#pragma once

#include <cstddef>
#include <ctime>

#include "dashboard/plugin_base.hpp"
#include "plugins/calendar_model.hpp"
#include "plugins/graph_calendar_provider.hpp"

namespace plugins {

class CalendarPlugin final : public dashboard::PluginBase {
  public:
    CalendarPlugin();

    uint32_t refreshIntervalMs() const override;

    /// Tenant + client ID, applied live from Settings. Neither is a secret — see
    /// graph_calendar_provider.hpp for why device-code sign-in needs no client secret at all.
    void setAccount(const char* tenant, const char* client_id);

    /// Headline is the auth state or the next meeting's countdown; the supporting line is the
    /// meeting's title and room, or a prompt to sign in.
    void summarise(dashboard::PluginSummary& out) const override;

  protected:
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;

    /// One OAuth round-trip (a small, shallow JSON object) plus, once authorized, a calendarview
    /// response of modest depth — nowhere near GitHub's nested run/repo/commit objects, but the
    /// refresh-token buffer alone is a real 2 KB local, so this is sized like github's rather than
    /// like clock's.
    uint32_t workerStackBytes() const override { return 10240; }

  private:
    void buildAuthView(lv_obj_t* parent);
    void buildListView(lv_obj_t* parent);
    void showAuthView();
    void showListView();

    void renderAuth(const GraphAuthStatus& status);
    void renderList(const CalendarDay& day, std::time_t now_utc);

    static constexpr size_t kMaxRows = CalendarDay::kMaxEvents;

    GraphCalendarProvider provider_;

    /// Guarded by modelMutex().
    CalendarDay day_;
    GraphAuthStatus auth_status_;

    dashboard::MediumString tenant_;
    dashboard::MediumString client_id_;

    // ---- widgets --------------------------------------------------------------------------
    lv_obj_t* auth_view_ = nullptr;
    lv_obj_t* auth_heading_ = nullptr;
    lv_obj_t* auth_code_ = nullptr;
    lv_obj_t* auth_uri_ = nullptr;
    lv_obj_t* auth_note_ = nullptr;

    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* list_empty_ = nullptr;

    struct EventRow {
        lv_obj_t* root = nullptr;
        lv_obj_t* time_range = nullptr;
        lv_obj_t* subject = nullptr;
        lv_obj_t* location = nullptr;
    };
    EventRow rows_[kMaxRows];
};

}  // namespace plugins
