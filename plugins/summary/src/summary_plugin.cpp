#include "plugins/summary_plugin.hpp"

#include <cstring>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/theme.hpp"

namespace plugins {
namespace {

namespace theme = dashboard::theme;

constexpr const char* kTag = "summary";

/// Shown where a plugin has nothing to say. A dash reads as "nothing here"; an empty tile reads
/// as a bug.
constexpr const char* kNoData = "--";

/// Three across. With kGutter either side and kGapM between, a tile is ~390 px wide — enough for
/// a large value and a full line of supporting text without truncation.
constexpr size_t kTilesPerRow = 3;

/// Add a transparent, non-interactive filler to keep the last row's tiles the same width as the
/// rest. Without it a final row of two would flex to half the page each, and a grid whose bottom
/// row has wider boxes than its top row looks like a mistake rather than a layout.
void addRowFiller(lv_obj_t* row) {
    lv_obj_t* filler = lv_obj_create(row);
    theme::makePlain(filler);
    lv_obj_remove_flag(filler, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(filler, 1);
    lv_obj_set_height(filler, LV_PCT(100));
}

}  // namespace

SummaryPlugin::SummaryPlugin() : PluginBase("summary", "Dashboard") {}

uint32_t SummaryPlugin::refreshIntervalMs() const { return dash::cfg::kClockRefreshMs; }

void SummaryPlugin::setPages(dashboard::DashboardPlugin* const* pages, size_t count,
                             dashboard::PageHost* host) {
    host_ = host;
    tile_count_ = 0;
    for (size_t i = 0; i < count && tile_count_ < kMaxTiles; ++i) {
        if (pages[i] == nullptr || pages[i] == this) {
            continue;  // never a tile linking to this very page
        }
        tiles_[tile_count_++].plugin = pages[i];
    }
    if (tile_count_ < count) {
        // Say so rather than silently showing fewer pages than exist: a missing tile is
        // otherwise indistinguishable from a page that was never registered.
        ESP_LOGW(kTag, "%u pages offered, %u tiles shown (kMaxTiles = %u)",
                 static_cast<unsigned>(count), static_cast<unsigned>(tile_count_),
                 static_cast<unsigned>(kMaxTiles));
    }
}

esp_err_t SummaryPlugin::onInitialise() {
    if (tile_count_ == 0) {
        ESP_LOGW(kTag, "no pages to summarise; setPages() was not called before initialise()");
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void SummaryPlugin::buildBody(lv_obj_t* body) {
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, theme::kGapM, LV_PART_MAIN);

    lv_obj_t* row = nullptr;
    for (size_t i = 0; i < tile_count_; ++i) {
        if (i % kTilesPerRow == 0) {
            row = theme::makeRow(body);
            // flex_grow only, no explicit height: in a flex column the grow factor governs the
            // main axis, so setting both would leave a height that reads as meaningful and is not.
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_style_pad_column(row, theme::kGapM, LV_PART_MAIN);
        }
        buildTile(row, tiles_[i]);
    }

    // Pad the final row out to a full one so every tile is the same width.
    const size_t remainder = tile_count_ % kTilesPerRow;
    if (row != nullptr && remainder != 0) {
        for (size_t i = remainder; i < kTilesPerRow; ++i) {
            addRowFiller(row);
        }
    }
}

void SummaryPlugin::buildTile(lv_obj_t* parent, Tile& tile) {
    tile.card = theme::makeTapCard(parent);
    lv_obj_set_flex_grow(tile.card, 1);
    lv_obj_set_height(tile.card, LV_PCT(100));
    lv_obj_set_flex_flow(tile.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tile.card, theme::kGapS, LV_PART_MAIN);

    // The plugin pointer rides on the widget rather than the Tile, so the click handler needs
    // nothing but the event — no index arithmetic that could drift out of step with the array.
    lv_obj_add_event_cb(
        tile.card,
        [](lv_event_t* event) {
            auto* self = static_cast<SummaryPlugin*>(lv_event_get_user_data(event));
            auto* target = static_cast<dashboard::DashboardPlugin*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(event))));
            if (self != nullptr && self->host_ != nullptr && target != nullptr) {
                self->host_->showById(target->id());
            }
        },
        LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(tile.card, tile.plugin);

    // Title row: the page's own name, with its state dot beside it. The dot is the same signal
    // the page's header carries, so a stale tile is visibly stale without opening it.
    lv_obj_t* title_row = theme::makeRow(tile.card);
    lv_obj_set_style_pad_column(title_row, theme::kGapS, LV_PART_MAIN);
    tile.dot = theme::makeStatusDot(title_row);
    tile.title = theme::makeLabel(title_row, tile.plugin->title(), theme::fontLabel(),
                                  theme::textMuted());
    lv_obj_set_flex_grow(tile.title, 1);
    lv_label_set_long_mode(tile.title, LV_LABEL_LONG_DOT);

    // The value, and the qualifier under it. fontDisplay for the headline: this is the number
    // the page exists to show, and the tile is read from further away than the page is.
    tile.primary = theme::makeLabel(tile.card, kNoData, theme::fontDisplay(),
                                    theme::textPrimary());
    lv_obj_set_width(tile.primary, LV_PCT(100));
    lv_label_set_long_mode(tile.primary, LV_LABEL_LONG_DOT);

    tile.secondary = theme::makeLabel(tile.card, "", theme::fontBody(), theme::textSecondary());
    lv_obj_set_width(tile.secondary, LV_PCT(100));
    lv_label_set_long_mode(tile.secondary, LV_LABEL_LONG_WRAP);
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t SummaryPlugin::fetch(bool force) {
    (void)force;
    // Nothing to fetch — every tile's data belongs to another plugin, which fetches it on its
    // own schedule. See the note at the top of the header.
    return ESP_OK;
}

void SummaryPlugin::updateUi() {
    for (size_t i = 0; i < tile_count_; ++i) {
        refreshTile(tiles_[i]);
    }
}

void SummaryPlugin::onTick() {
    // Every tick, not only on refresh: the tiles' contents belong to other plugins, whose data
    // changes without this page being told. A countdown that only moved when this page fetched
    // would be wrong for minutes at a time.
    for (size_t i = 0; i < tile_count_; ++i) {
        refreshTile(tiles_[i]);
    }
}

void SummaryPlugin::refreshTile(Tile& tile) {
    if (tile.plugin == nullptr || tile.primary == nullptr) {
        return;
    }

    dashboard::PluginSummary summary;
    tile.plugin->summarise(summary);
    const dashboard::DataState state = tile.plugin->state();

    // Nothing below this line touches a widget unless the value actually changed — see the note
    // on Tile::shown_primary.
    const bool first = !tile.shown_valid;
    if (first || summary.primary != tile.shown_primary) {
        lv_label_set_text(tile.primary,
                          summary.primary.empty() ? kNoData : summary.primary.c_str());
        tile.shown_primary = summary.primary;
    }
    if (first || summary.secondary != tile.shown_secondary) {
        lv_label_set_text(tile.secondary, summary.secondary.c_str());
        tile.shown_secondary = summary.secondary;
    }
    if (first || state != tile.shown_state) {
        theme::setStatusDot(tile.dot, state);
        tile.shown_state = state;
    }
    tile.shown_valid = true;
}

}  // namespace plugins
