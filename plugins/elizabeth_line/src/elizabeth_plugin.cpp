#include "plugins/elizabeth_plugin.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/storage/cache_store.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace theme = dashboard::theme;
namespace timeutil = dashboard::timeutil;
using dashboard::storage::CacheStore;

constexpr const char* kTag = "elizabeth";

/// Cache key. Only the STATUS is cached, deliberately — see loadCachedStatus().
constexpr const char* kStatusCacheKey = "tflstatus";

/// Local minute at which the board turns round. 12:00.
constexpr int kMiddayMinutes = 12 * 60;

/// Column widths for the board. Fixed rather than flexed, because a departure board's columns
/// lining up down the page is most of what makes it read as one.
constexpr int32_t kTimeColumn = 150;
constexpr int32_t kPlatformColumn = 170;
constexpr int32_t kCountdownColumn = 190;

constexpr int32_t kBoardRowHeight = 62;

constexpr const char* kNoData = "--";

/// Amber, as on the real thing. Not from the theme palette: this is the one place in the product
/// deliberately imitating something else, and it should not drift when the theme changes.
lv_color_t boardAmber() { return lv_color_hex(0xF5A623); }

lv_color_t colourForLevel(ServiceLevel level) {
    switch (level) {
        case ServiceLevel::Good:
            return theme::ok();
        case ServiceLevel::Minor:
            return theme::stale();
        case ServiceLevel::Severe:
        case ServiceLevel::Closed:
            return theme::error();
        case ServiceLevel::Unknown:
        default:
            return theme::textMuted();
    }
}

/// "due", "1 min", "12 min". Boards say "due" rather than "0 min" and so does this.
void formatCountdown(char* out, size_t capacity, int32_t seconds) {
    if (seconds < 30) {
        std::snprintf(out, capacity, "due");
        return;
    }
    // Round to nearest rather than truncating: 119 seconds is very nearly 2 minutes, and a board
    // that says "1 min" for it is the reason people run.
    const int32_t minutes = (seconds + 30) / 60;
    std::snprintf(out, capacity, "%ld min", static_cast<long>(minutes));
}

void formatLocalTime(char* out, size_t capacity, std::time_t when) {
    std::tm local = {};
    if (when <= 0 || localtime_r(&when, &local) == nullptr) {
        std::snprintf(out, capacity, "%s", kNoData);
        return;
    }
    timeutil::formatTime24h(out, capacity, local, /*with_seconds=*/false);
}

}  // namespace

ElizabethPlugin::ElizabethPlugin() : PluginBase("elizabeth", "Elizabeth line") {}

// ---------------------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------------------

bool ElizabethPlugin::inCommuteWindow() const {
    const int minutes = timeutil::localMinutesSinceMidnight();
    if (minutes < 0) {
        return false;  // clock not set; assume the quiet cadence
    }
    return timeutil::inTimeWindow(minutes, morning_start_, morning_end_) ||
           timeutil::inTimeWindow(minutes, evening_start_, evening_end_);
}

uint32_t ElizabethPlugin::refreshIntervalMs() const {
    return inCommuteWindow() ? dash::cfg::kTflCommuteRefreshMs : dash::cfg::kTflIdleRefreshMs;
}

Journey ElizabethPlugin::journeyForNow() {
    const int minutes = timeutil::localMinutesSinceMidnight();
    if (minutes < 0) {
        // Clock unknown. Pick the morning board: a device that has just booted is more likely to
        // be starting a day than ending one, and the next tick after time sync corrects it anyway.
        return Journey::ToLiverpoolStreet;
    }
    return (minutes < kMiddayMinutes) ? Journey::ToLiverpoolStreet : Journey::ToAbbeyWood;
}

void ElizabethPlugin::setCommuteWindows(int32_t morning_start, int32_t morning_end,
                                        int32_t evening_start, int32_t evening_end) {
    morning_start_ = morning_start;
    morning_end_ = morning_end;
    evening_start_ = evening_start;
    evening_end_ = evening_end;
}

// ---------------------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------------------

esp_err_t ElizabethPlugin::onInitialise() {
    loadCachedStatus();
    return ESP_OK;
}

void ElizabethPlugin::loadCachedStatus() {
    // Only the status is cached, and departures deliberately are not: a departure board is worth
    // nothing the moment it is stale. Showing "3 min" for a train that left before the device was
    // switched off would be actively misleading, whereas "there were severe delays when we last
    // looked" is still information.
    if (!CacheStore::has(kStatusCacheKey)) {
        return;
    }

    dashboard::net::ResponseBuffer buffer(8 * 1024);
    if (!buffer.valid()) {
        return;
    }

    size_t length = 0;
    if (CacheStore::get(kStatusCacheKey, buffer.data(), buffer.capacity(), &length) != ESP_OK ||
        length == 0) {
        return;
    }

    LineStatus cached;
    if (!parseLineStatus(buffer.data(), length, cached)) {
        ESP_LOGW(kTag, "cached status did not parse; discarding it");
        CacheStore::remove(kStatusCacheKey);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(modelMutex());
        status_ = cached;
    }
    noteCachedData(static_cast<std::time_t>(CacheStore::timestamp(kStatusCacheKey)));
    ESP_LOGI(kTag, "restored cached status: %s", cached.description.c_str());
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void ElizabethPlugin::buildBody(lv_obj_t* body) {
    buildStatusCard(body);
    buildBoard(body);
}

void ElizabethPlugin::buildStatusCard(lv_obj_t* parent) {
    lv_obj_t* card = theme::makeCard(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, theme::kGapS, LV_PART_MAIN);

    lv_obj_t* headline_row = theme::makeRow(card);
    lv_obj_set_style_pad_column(headline_row, theme::kGapM, LV_PART_MAIN);

    status_dot_ = theme::makeStatusDot(headline_row);
    // Larger than the header's dot: this one is the page's primary signal, not a decoration on a
    // title bar, and it has to read from across the room alongside 40 px text.
    lv_obj_set_size(status_dot_, theme::kStatusDotSize * 2, theme::kStatusDotSize * 2);

    status_headline_ =
        theme::makeLabel(headline_row, "Checking...", theme::fontDisplay(), theme::textPrimary());

    // The disruption text. Bounded on purpose: an observed message ran to 600 characters including
    // a list of every operator accepting tickets, and letting it grow without limit would push the
    // departure board off the bottom of the screen. Three lines carries the substantive part, and
    // LONG_DOT makes the truncation visible rather than silent.
    status_reason_ = theme::makeLabel(card, "", theme::fontLabel(), theme::textSecondary());
    lv_obj_set_width(status_reason_, LV_PCT(100));
    lv_label_set_long_mode(status_reason_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(status_reason_, lv_font_get_line_height(theme::fontLabel()) * 3);
    lv_obj_add_flag(status_reason_, LV_OBJ_FLAG_HIDDEN);
}

void ElizabethPlugin::buildBoard(lv_obj_t* parent) {
    lv_obj_t* card = theme::makeCard(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, theme::kGapXs, LV_PART_MAIN);
    // Darker than a normal card. A departure board is light-on-black, and dropping the panel to the
    // page background is what separates it from the status card above it.
    lv_obj_set_style_bg_color(card, theme::bg(), LV_PART_MAIN);

    board_title_ = theme::makeLabel(card, "", theme::fontBody(), theme::textMuted());
    theme::makeSeparator(card);

    for (size_t i = 0; i < BoardData::kMaxDepartures; ++i) {
        BoardRow& row = rows_[i];
        row.root = theme::makeRow(card);
        lv_obj_set_height(row.root, kBoardRowHeight);
        lv_obj_set_style_pad_column(row.root, theme::kGapM, LV_PART_MAIN);

        // Time and countdown are fixed-width so the columns line up down the board; the
        // destination takes whatever is left, which is what lets a long name run on without
        // pushing the countdown out of alignment.
        row.time = theme::makeLabel(row.root, "", theme::fontTitle(), theme::textPrimary());
        lv_obj_set_width(row.time, kTimeColumn);

        row.destination = theme::makeLabel(row.root, "", theme::fontTitle(), theme::textPrimary());
        lv_obj_set_flex_grow(row.destination, 1);
        lv_label_set_long_mode(row.destination, LV_LABEL_LONG_DOT);

        row.platform = theme::makeLabel(row.root, "", theme::fontBody(), theme::textMuted());
        lv_obj_set_width(row.platform, kPlatformColumn);

        row.countdown = theme::makeLabel(row.root, "", theme::fontTitle(), boardAmber());
        lv_obj_set_width(row.countdown, kCountdownColumn);
        lv_obj_set_style_text_align(row.countdown, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
    }

    // Shown instead of the rows when the feed returns nothing — after the last train, or during a
    // suspension. An empty board with no explanation reads as a broken page.
    board_empty_ = theme::makeLabel(card, "No departures listed", theme::fontBody(),
                                    theme::textMuted());
    lv_obj_add_flag(board_empty_, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t ElizabethPlugin::fetch(bool force) {
    (void)force;

    const Journey journey = journeyForNow();

    // One buffer, reused for both requests. From PSRAM, and released when this returns — a 24 KB
    // array on the plugin object would sit in internal SRAM for the life of the device, which is
    // the scarce resource here. See docs/BACKLOG.md §1.3.
    dashboard::net::ResponseBuffer buffer(TflProvider::kResponseBytes);
    if (!buffer.valid()) {
        setError("out of memory");
        return ESP_ERR_NO_MEM;
    }

    // The two calls are independent and each is allowed to fail on its own. The refresh only fails
    // outright when BOTH did, so a TfL status blip cannot blank a perfectly good departure board.
    LineStatus status;
    const esp_err_t status_err = provider_.fetchStatus(buffer.data(), buffer.capacity(), status);
    if (status_err == ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(modelMutex());
            status_ = status;
        }
        // Cache the raw body while the buffer still holds it — it is about to be reused for the
        // arrivals call and freed on return.
        const esp_err_t cache_err =
            CacheStore::put(kStatusCacheKey, buffer.data(), std::strlen(buffer.data()));
        if (cache_err != ESP_OK) {
            ESP_LOGW(kTag, "could not cache the status: %s", esp_err_to_name(cache_err));
        }
    }

    BoardData board;
    const esp_err_t board_err =
        provider_.fetchBoard(journey, buffer.data(), buffer.capacity(), board);
    if (board_err == ESP_OK) {
        std::lock_guard<std::mutex> lock(modelMutex());
        board_ = board;
        journey_ = journey;
        board_fetched_utc_ = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;
    }

    if (status_err != ESP_OK && board_err != ESP_OK) {
        setError(provider_.lastError());
        return board_err;
    }
    if (board_err != ESP_OK) {
        // Departures are the reason to look at this page, so their absence is worth saying even
        // though the refresh is being reported as a success.
        setError("no live departures");
    } else if (status_err != ESP_OK) {
        setError("line status unavailable");
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------

void ElizabethPlugin::updateUi() {
    LineStatus status;
    BoardData board;
    Journey journey;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        status = status_;
        board = board_;
        journey = journey_;
    }

    renderStatus(status);
    renderBoard(board, journey);
}

void ElizabethPlugin::renderStatus(const LineStatus& status) {
    if (status_headline_ == nullptr || status_reason_ == nullptr || status_dot_ == nullptr) {
        return;
    }

    if (!status.valid) {
        lv_label_set_text(status_headline_, "Service status unavailable");
        lv_obj_set_style_text_color(status_headline_, theme::textMuted(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(status_dot_, theme::textMuted(), LV_PART_MAIN);
        lv_obj_add_flag(status_reason_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const lv_color_t colour = colourForLevel(status.level);
    lv_label_set_text(status_headline_, status.description.c_str());
    lv_obj_set_style_text_color(status_headline_, colour, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_dot_, colour, LV_PART_MAIN);

    // Shown only when there is something wrong AND something to say. On a good service the space
    // stays empty: standing text that always says "everything is fine" is text nobody reads, and
    // its absence is what makes its presence noticeable.
    if (status.warning() && !status.reason.empty()) {
        lv_label_set_text(status_reason_, status.reason.c_str());
        lv_obj_remove_flag(status_reason_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(status_reason_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ElizabethPlugin::renderBoard(const BoardData& board, Journey journey) {
    if (board_title_ == nullptr || board_empty_ == nullptr) {
        return;
    }

    char title[96];
    std::snprintf(title, sizeof(title), "%s to %s", journeyOrigin(journey),
                  journeyDestination(journey));
    lv_label_set_text(board_title_, title);

    if (board.count == 0) {
        lv_obj_remove_flag(board_empty_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(board_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < BoardData::kMaxDepartures; ++i) {
        BoardRow& row = rows_[i];
        if (row.root == nullptr) {
            return;
        }
        if (i >= board.count) {
            lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row.root, LV_OBJ_FLAG_HIDDEN);

        const Departure& departure = board.departures[i];

        char text[64];
        formatLocalTime(text, sizeof(text), departure.expected_utc);
        lv_label_set_text(row.time, text);

        lv_label_set_text(row.destination, departure.destination.c_str());

        if (departure.platform.empty()) {
            lv_label_set_text(row.platform, "");
        } else {
            std::snprintf(text, sizeof(text), "Plat %s", departure.platform.c_str());
            lv_label_set_text(row.platform, text);
        }
    }

    renderCountdowns();
}

void ElizabethPlugin::renderCountdowns() {
    BoardData board;
    std::time_t fetched = 0;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        board = board_;
        fetched = board_fetched_utc_;
    }

    // Age the fetched countdown by however long ago it was fetched, so the board ticks down
    // between refreshes instead of freezing for two minutes at a time. With no valid clock there
    // is no elapsed time to compute, so the fetched value stands.
    long long elapsed = 0;
    if (fetched > 0 && timeutil::systemTimeValid()) {
        elapsed = static_cast<long long>(timeutil::nowUtc()) - static_cast<long long>(fetched);
        if (elapsed < 0) {
            elapsed = 0;
        }
    }

    for (size_t i = 0; i < BoardData::kMaxDepartures; ++i) {
        if (rows_[i].countdown == nullptr) {
            return;
        }
        if (i >= board.count) {
            continue;
        }
        long long remaining = static_cast<long long>(board.departures[i].seconds_away) - elapsed;
        if (remaining < 0) {
            remaining = 0;
        }
        char text[24];
        formatCountdown(text, sizeof(text), static_cast<int32_t>(remaining));
        lv_label_set_text(rows_[i].countdown, text);
    }
}

void ElizabethPlugin::onTick() {
    // Midday, or the clock arriving after boot, turns the board round. Refetch rather than relabel:
    // the departures on screen are from the other end of the line and none of them apply.
    const Journey wanted = journeyForNow();
    if (!requested_journey_valid_) {
        // First tick. Record where we are without asking for anything — the ordinary refresh cycle
        // is about to fetch this direction anyway, and a second request here would just be
        // coalesced away.
        requested_journey_ = wanted;
        requested_journey_valid_ = true;
    } else if (wanted != requested_journey_) {
        ESP_LOGI(kTag, "board turning round: now showing %s to %s", journeyOrigin(wanted),
                 journeyDestination(wanted));
        requested_journey_ = wanted;
        refresh(/*force=*/true);
        return;
    }

    renderCountdowns();
}

}  // namespace plugins
