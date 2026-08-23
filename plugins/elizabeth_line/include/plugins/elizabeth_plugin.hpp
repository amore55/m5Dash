// Page 3: the Elizabeth line.
//
// Two things on one screen, in the order they matter when you are about to leave the house:
//
//   1. Is the line running? The status, in TfL's own words, coloured by severity — and when there
//      IS something wrong, what it is. On a good service the explanation area is empty rather than
//      reassuring, because a line of text saying nothing is happening is a line you learn to skip.
//   2. When is the next train? A departure board in the style of the real thing: expected time,
//      destination, platform, and how many minutes away.
//
// THE BOARD TURNS ROUND AT 13:30. Before then it shows Abbey Wood to Liverpool Street, the way in;
// after, Liverpool Street to Abbey Wood, the way home. That is the whole rule — it is not tied to
// the commute windows in Settings, which control how OFTEN the page refreshes. Tying it to those
// would leave the board showing nothing useful at 06:30 or 21:00, when the answer to "which way am
// I going" is still perfectly obvious.
//
// EITHER BOARD CAN BE ASKED FOR BY HAND, with the two station buttons beside the status box. A
// manual choice holds until the schedule next changes its own mind — that is, until 13:30 or
// midnight passes — and then the automatic rule takes over again. The alternative designs are both
// worse: an override that never expires means one afternoon tap silently shows the wrong way home
// for the rest of the week, and one that expires on a timer takes the board away from you while you
// are still looking at it.
//
// The countdown re-renders every tick from the fetched seconds, so "4 min" becomes "3 min" without
// waiting for the next refresh — which is what makes it read like a board rather than a snapshot.

#pragma once

#include <cstddef>
#include <ctime>

#include "dashboard/plugin_base.hpp"
#include "plugins/elizabeth_model.hpp"
#include "plugins/tfl_provider.hpp"

namespace plugins {

class ElizabethPlugin final : public dashboard::PluginBase {
  public:
    ElizabethPlugin();

    /// Commute-aware: the fast interval inside a configured window, the slow one outside.
    uint32_t refreshIntervalMs() const override;

    /// Applied live from Settings. Minutes since local midnight; a window where start == end is
    /// disabled, which timeutil::inTimeWindow() already handles.
    void setCommuteWindows(int32_t morning_start, int32_t morning_end, int32_t evening_start,
                           int32_t evening_end);

  protected:
    esp_err_t onInitialise() override;
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;
    void onTick() override;

    /// Two sequential HTTPS requests plus JSON parsing of a 15 KB response. The response itself is
    /// in PSRAM (see ResponseBuffer), so this only has to cover cJSON's own recursion.
    uint32_t workerStackBytes() const override { return 8192; }

    /// Headline is the line status; the supporting line is how long until the next train off
    /// whichever board is currently showing.
    void summarise(dashboard::PluginSummary& out) const override;

  private:
    void buildStatusCard(lv_obj_t* parent);
    void buildBoard(lv_obj_t* parent);

    /// The two station buttons, laid out in line with the status box.
    void buildStationButtons(lv_obj_t* parent);

    /// A station button was pressed. Sets the manual override and refetches.
    void selectJourney(Journey journey);

    /// Paint the selected state onto the two buttons. Cheap; safe to call whenever.
    void updateStationButtons(Journey showing);

    void renderStatus(const LineStatus& status);
    void renderBoard(const BoardData& board, Journey journey);
    /// Just the countdown column, cheap enough to run on every 250 ms tick.
    void renderCountdowns();

    void loadCachedStatus();

    /// Which way round the CLOCK says the board should be. Ignores any manual override.
    static Journey scheduledJourney();

    /// Which way round the board should actually be: the manual choice if one is standing,
    /// otherwise the scheduled one.
    Journey journeyForNow() const;

    /// True when the local time is inside either configured commute window.
    bool inCommuteWindow() const;

    TflProvider provider_;

    /// Guarded by modelMutex().
    LineStatus status_;
    BoardData board_;
    Journey journey_ = Journey::ToLiverpoolStreet;

    /// When the board's seconds_away values were taken, so the countdown can be aged on the UI
    /// thread without re-fetching. Set on the worker, read on the LVGL thread.
    std::time_t board_fetched_utc_ = 0;

    int32_t morning_start_ = 7 * 60;
    int32_t morning_end_ = 9 * 60 + 30;
    int32_t evening_start_ = 16 * 60 + 30;
    int32_t evening_end_ = 19 * 60;

    /// The direction onTick() has already asked for. ONLY onTick() writes these.
    ///
    /// Deliberately not "the direction currently on screen": comparing the clock against what is
    /// rendered means that between asking for the turn-round and the fetch landing, every tick sees
    /// a mismatch and asks again. The first build did exactly that and logged the turn-round four
    /// times in eight seconds. Recording what was *requested* makes the transition edge-triggered.
    Journey requested_journey_ = Journey::ToLiverpoolStreet;
    bool requested_journey_valid_ = false;

    /// A board asked for by hand, overriding the clock until the schedule next changes.
    /// LVGL thread only — set by a button press, cleared by onTick().
    Journey manual_journey_ = Journey::ToLiverpoolStreet;
    bool manual_journey_valid_ = false;

    /// What scheduledJourney() said on the previous tick, so a CHANGE in it can be detected and
    /// used to retire the manual override. Without this the override could only be cleared on a
    /// timer or never, and both alternatives are worse — see the note at the top of this file.
    Journey last_scheduled_ = Journey::ToLiverpoolStreet;
    bool last_scheduled_valid_ = false;

    // ---- widgets ------------------------------------------------------------------------
    lv_obj_t* status_headline_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* status_reason_ = nullptr;

    lv_obj_t* board_title_ = nullptr;
    lv_obj_t* board_empty_ = nullptr;

    /// Labelled by the station you are standing at, because that is what you choose between when
    /// you look at a board. "Liv St" shows departures FROM Liverpool Street.
    lv_obj_t* liverpool_button_ = nullptr;
    lv_obj_t* abbey_button_ = nullptr;

    /// What the buttons are currently painted as, so a 250 ms tick does not restyle them
    /// — and dirty them for redraw — when nothing has changed.
    Journey buttons_showing_ = Journey::ToLiverpoolStreet;
    bool buttons_showing_valid_ = false;

    struct BoardRow {
        lv_obj_t* root = nullptr;
        lv_obj_t* time = nullptr;
        lv_obj_t* destination = nullptr;
        lv_obj_t* platform = nullptr;
        lv_obj_t* status = nullptr;
        lv_obj_t* countdown = nullptr;
    };
    BoardRow rows_[BoardData::kMaxDepartures];
};

}  // namespace plugins
