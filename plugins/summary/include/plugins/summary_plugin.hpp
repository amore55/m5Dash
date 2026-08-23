// The hub page: every other page, at a glance, in its own tile.
//
// This page owns no data. It asks each registered plugin for two lines via
// DashboardPlugin::summarise() and lays them out as tap targets — press a tile and you land on
// that page; press the home icon there and you come back. So the tiles are always exactly as
// fresh as the pages behind them, and adding a seventh page adds a seventh tile with no work
// here. That is the whole reason summarise() lives on the plugin interface rather than this page
// holding a switch on plugin id.
//
// WHAT IT DELIBERATELY DOES NOT DO: it does not fetch, and it does not drive anyone else's
// refresh. Every plugin already refreshes on its own schedule whether visible or not
// (refreshWhenHidden), so a summary tile showing a stale number means that page's own data is
// stale — which is worth seeing, and is exactly what its state dot says. Making this page force
// refreshes would multiply every API call by the number of times someone glances at the hub.

#pragma once

#include <cstddef>

#include "dashboard/plugin_base.hpp"

namespace plugins {

class SummaryPlugin final : public dashboard::PluginBase {
  public:
    /// Tiles fit comfortably across 1280 px in two rows of three. More than six would need a
    /// smaller tile than is worth reading from across a desk, which is the point of the page.
    static constexpr size_t kMaxTiles = 6;

    SummaryPlugin();

    /// The pages to summarise, and the host used to navigate to them.
    ///
    /// Pointers are stored, not copied: every plugin here is a file-scope object in app_main
    /// with the same lifetime as the application. Must be called BEFORE createPage(), because
    /// the tiles are built once from this list.
    ///
    /// This plugin must not be in its own list — a tile linking to the page you are on is at
    /// best confusing, and setPages() filters it out by pointer identity anyway.
    void setPages(dashboard::DashboardPlugin* const* pages, size_t count,
                  dashboard::PageHost* host);

    /// Nothing is fetched, so the interval only governs how often the (cheap) state is
    /// re-evaluated. The tiles themselves update every tick.
    uint32_t refreshIntervalMs() const override;

  protected:
    esp_err_t onInitialise() override;
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;
    void onTick() override;

    /// No network, no JSON, no filesystem. The worker exists only because PluginBase always
    /// starts one; it does nothing but return.
    uint32_t workerStackBytes() const override { return 2560; }

  private:
    struct Tile {
        dashboard::DashboardPlugin* plugin = nullptr;
        lv_obj_t* card = nullptr;
        lv_obj_t* dot = nullptr;
        lv_obj_t* title = nullptr;
        lv_obj_t* primary = nullptr;
        lv_obj_t* secondary = nullptr;

        /// What is currently on screen, so an unchanged tile is not re-set every 250 ms.
        /// lv_label_set_text() copies and invalidates unconditionally, so this guard is the
        /// difference between six labels redrawing four times a second and none of them.
        dashboard::ShortString shown_primary;
        dashboard::MediumString shown_secondary;
        dashboard::DataState shown_state = dashboard::DataState::Idle;
        bool shown_valid = false;
    };

    void buildTile(lv_obj_t* parent, Tile& tile);
    void refreshTile(Tile& tile);

    Tile tiles_[kMaxTiles];
    size_t tile_count_ = 0;

    dashboard::PageHost* host_ = nullptr;
};

}  // namespace plugins
