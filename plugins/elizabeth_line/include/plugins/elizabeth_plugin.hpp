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
// THE BOARD TURNS ROUND AT MIDDAY. Before noon it shows Abbey Wood to Liverpool Street, the way in;
// after noon, Liverpool Street to Abbey Wood, the way home. That is the whole rule — it is not tied
// to the commute windows in Settings, which control how OFTEN the page refreshes. Tying it to those
// would leave the board showing nothing useful at 06:30 or 21:00, when the answer to "which way am
// I going" is still perfectly obvious.
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

  private:
    void buildStatusCard(lv_obj_t* parent);
    void buildBoard(lv_obj_t* parent);

    void renderStatus(const LineStatus& status);
    void renderBoard(const BoardData& board, Journey journey);
    /// Just the countdown column, cheap enough to run on every 250 ms tick.
    void renderCountdowns();

    void loadCachedStatus();

    /// Which way round the board should be, right now. Reads the clock.
    static Journey journeyForNow();

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

    // ---- widgets ------------------------------------------------------------------------
    lv_obj_t* status_headline_ = nullptr;
    lv_obj_t* status_dot_ = nullptr;
    lv_obj_t* status_reason_ = nullptr;

    lv_obj_t* board_title_ = nullptr;
    lv_obj_t* board_empty_ = nullptr;

    struct BoardRow {
        lv_obj_t* root = nullptr;
        lv_obj_t* time = nullptr;
        lv_obj_t* destination = nullptr;
        lv_obj_t* platform = nullptr;
        lv_obj_t* countdown = nullptr;
    };
    BoardRow rows_[BoardData::kMaxDepartures];
};

}  // namespace plugins
