// The dashboard plugin contract.
//
// THREADING CONTRACT — this is the rule the whole application hangs off, and breaking it is
// how an embedded LVGL UI turns into a stuttering mess or a crash:
//
//   * createPage(), onShow(), onHide(), tick() and refresh() are ALL called on the LVGL
//     thread, with the LVGL lock already held. Widget access from them is safe and must NOT
//     take the lock again.
//   * refresh(force) must NOT do work. It may only post a job to the plugin's own worker
//     task and return promptly. PluginBase implements this correctly; implement it yourself
//     only if you have read PluginBase first.
//   * HTTPS, JSON parsing and filesystem access happen on the plugin's worker task, which
//     must never touch an lv_obj_t.
//   * Handoff between the two is a mutex-guarded model plus an atomic dirty flag. tick() is
//     where the model is copied into widgets.
//
// A plugin that violates the contract does not just slow itself down: it blocks every other
// page, because there is one LVGL thread.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

#include "dashboard/fixed_string.hpp"

namespace dashboard {

/// What a page says about itself on the summary tile: one headline and one supporting line.
///
/// Fixed capacity, because these are filled on the LVGL thread every tick and must not allocate.
/// Both may be left empty — the summary page renders a dash rather than a blank tile.
struct PluginSummary {
    /// The number or state you would read from across the room: "18°C", "07:42", "Good Service".
    ShortString primary;

    /// The qualifier: "Partly cloudy", "23/08/2026", "Next train 4 min".
    MediumString secondary;
};

/// Lifecycle of a plugin's data. Drives the header status dot and the footer text, so the
/// user can always tell the difference between "loading", "this is old" and "this is broken".
enum class DataState : uint8_t {
    /// Nothing fetched yet and nothing attempted.
    Idle,
    /// A fetch is in flight. Only shown when there is no previous data to display.
    Loading,
    /// Fresh, successful data.
    Ok,
    /// Last fetch failed but cached data is being shown. The user sees the data AND that it
    /// is old — never a blank page, never a silent lie.
    Stale,
    /// Failed with nothing worth displaying.
    Error,
    /// Turned off in settings, or missing required configuration (no API token, etc.).
    Disabled,
};

const char* toString(DataState state);

class DashboardPlugin {
  public:
    virtual ~DashboardPlugin() = default;

    /// Stable identifier used in settings, page ordering and the OTA-safe config schema.
    /// Must be a compile-time constant string: it is persisted, so it cannot change casually.
    virtual const char* id() const = 0;

    /// Human-readable page title shown in the header.
    virtual const char* title() const = 0;

    /// One-time setup: start the worker task, open storage, load cached data.
    /// Called before createPage(). Returning anything other than ESP_OK marks the plugin
    /// Disabled — it is skipped, and the rest of the dashboard carries on regardless.
    virtual esp_err_t initialise() = 0;

    /// Build the page's widget tree under `parent`. `parent` is a full-screen container owned
    /// by PageManager; the plugin owns everything it creates inside it.
    ///
    /// Called exactly once. PageManager keeps pages alive and toggles visibility rather than
    /// destroying and rebuilding, so a plugin cannot leak by being navigated to repeatedly.
    virtual void createPage(lv_obj_t* parent) = 0;

    /// The page became / stopped being visible. Use these to start and stop anything
    /// expensive — a per-second animation, a high-rate timer.
    virtual void onShow() = 0;
    virtual void onHide() = 0;

    /// Ask for new data. MUST NOT BLOCK — see the threading contract above.
    /// `force` distinguishes a user-initiated pull-to-refresh (which should bypass any
    /// "fetched recently" suppression) from the scheduler's periodic tick.
    virtual void refresh(bool force) = 0;

    /// Called every dash::cfg::kUiTickPeriodMs on the LVGL thread, whether visible or not.
    /// Must stay cheap: it runs for every registered plugin on every tick.
    virtual void tick() = 0;

    /// How often the scheduler should call refresh(false). May vary at runtime — the
    /// Elizabeth line plugin returns a shorter interval during commute hours.
    virtual uint32_t refreshIntervalMs() const = 0;

    /// Whether the scheduler should keep refreshing while the page is not visible.
    /// True for anything that should be up to date the instant you swipe to it. False for
    /// pages whose data is only meaningful on screen.
    virtual bool refreshWhenHidden() const { return true; }

    /// Network connectivity changed. Plugins typically use this to refresh immediately on
    /// reconnect rather than waiting out the remainder of their interval.
    virtual void onNetworkChanged(bool online) { (void)online; }

    virtual DataState state() const = 0;

    /// Two lines for the summary page's tile: what this page would want you to know at a glance,
    /// without opening it.
    ///
    /// Called ON THE LVGL THREAD from the summary page's tick, for every plugin whether visible
    /// or not. So it must be cheap, and it must take the plugin's own model mutex — it reads the
    /// same fields updateUi() does, and the worker thread may be writing them.
    ///
    /// The default leaves both lines empty, which renders as a dash. Deliberate: a plugin with
    /// nothing to say should not be made to invent something, and a new page gets a working tile
    /// before anyone writes its summary.
    virtual void summarise(PluginSummary& out) const { (void)out; }
};

/// The subset of PageManager that a plugin is allowed to drive.
///
/// Exists so the Settings page can close itself and re-order pages without plugins depending
/// on PageManager's full surface (and without a global).
class PageHost {
  public:
    virtual ~PageHost() = default;

    /// Dismiss an out-of-rotation page (Settings) and return to the previous one.
    virtual void closeOverlay() = 0;

    /// Show a page by id. No-op if the id is unknown or the plugin is disabled.
    virtual void showById(const char* id) = 0;

    /// Force a refresh of the currently visible page, as pull-to-refresh does.
    virtual void requestRefresh() = 0;

    /// Position within the rotation, for the page indicator.
    virtual size_t rotationCount() const = 0;
    virtual size_t rotationIndex() const = 0;

    /// Re-read page order / enabled flags from settings and rebuild the indicators.
    /// Called by the Settings page after the user changes them.
    virtual void reloadPageConfiguration() = 0;
};

}  // namespace dashboard
