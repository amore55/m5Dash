// Page: To-dos. Local to this device; the network side is TelegramService, not this page.
//
// WHY fetch() DOES NO NETWORKING
//
// TaskStore is a shared, already-loaded object (app_main loads it once at boot) that
// TelegramService mutates from ITS OWN background task whenever a command arrives. This page's
// job is only to notice and redraw — "UI reconcile", which is what
// dash::cfg::kTodosRefreshMs was already named for. TelegramService also calls back into this
// page (see setChangedCallback()) so a message answered a moment ago appears promptly, rather
// than waiting out the full reconcile interval.
//
// requiresNetwork() is FALSE: the task list is entirely local, and reading it works exactly the
// same with the Wi-Fi off — only ADDING one via Telegram needs the network, which is
// TelegramService's concern, not this page's.
//
// ON-DEVICE INTERACTION is deliberately narrow — tap a row to complete it, tap a delete control
// twice within a few seconds to remove it — because TYPING happens through Telegram. That is the
// entire reason Telegram is the input method at all: an on-screen keyboard for entering a task
// title is exactly what this design avoids.

#pragma once

#include <cstddef>
#include <ctime>

#include "dashboard/fixed_string.hpp"
#include "dashboard/plugin_base.hpp"
#include "dashboard/storage/task_store.hpp"

namespace plugins {

class TasksPlugin final : public dashboard::PluginBase {
  public:
    static constexpr size_t kMaxRows = 10;

    TasksPlugin();

    uint32_t refreshIntervalMs() const override;

    /// Must be called before initialise() — buildBody() has nothing to show without it, and
    /// fetch() has nothing to reconcile against.
    void setStore(dashboard::storage::TaskStore* store) { store_ = store; }

    /// Called by TelegramService (from ITS worker thread) after a command changes the store, so
    /// this page redraws promptly. Safe from any thread — it does no more than markDirty().
    void notifyChanged() { markDirty(); }

    /// Headline is the open count; the supporting line is the oldest open task's title, or "All
    /// clear" when there is nothing outstanding.
    void summarise(dashboard::PluginSummary& out) const override;

  protected:
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;
    void onTick() override;

    bool requiresNetwork() const override { return false; }
    /// No TLS, no JSON parsing — this only reads an already-loaded in-memory TaskStore.
    uint32_t workerStackBytes() const override { return 3072; }

  private:
    void renderList(const dashboard::storage::Task* rows, size_t count, size_t total_open);
    void onRowTapped(size_t row_index);
    void onDeleteTapped(size_t row_index);

    dashboard::storage::TaskStore* store_ = nullptr;

    /// Guarded by modelMutex().
    dashboard::storage::Task rows_snapshot_[kMaxRows];
    size_t rows_count_ = 0;
    size_t total_open_ = 0;

    /// Which row's delete control is armed, and until when. LVGL thread only — not part of the
    /// cross-thread model, since only a touch on THIS device can set or clear it.
    ///
    /// A SHARED single slot rather than a per-row timer: at most one row can be mid-confirm at a
    /// time, so tapping any other row's delete control (or letting this one expire) is all the
    /// "cancel" logic that is needed, without an LVGL timer object per row.
    dashboard::FixedString<24> armed_delete_id_;
    /// lv_tick_get() at the moment it was armed. Compared via lv_tick_elaps(), which is
    /// wraparound-safe — plain subtraction is not, and this counter wraps every ~49 days.
    uint32_t armed_delete_tick_ = 0;

    // ---- widgets --------------------------------------------------------------------------
    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* empty_label_ = nullptr;

    struct TaskRow {
        lv_obj_t* root = nullptr;
        lv_obj_t* title = nullptr;
        lv_obj_t* delete_btn = nullptr;
        lv_obj_t* delete_label = nullptr;
        /// Which task this row currently shows — the click handlers key off this, not the row
        /// index, so a tap lands on the right task even if the list was re-sorted between the
        /// row being built and the tap arriving.
        dashboard::FixedString<24> task_id;
    };
    TaskRow rows_[kMaxRows];
};

}  // namespace plugins
