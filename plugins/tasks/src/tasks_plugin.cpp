#include "plugins/tasks_plugin.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace theme = dashboard::theme;
namespace timeutil = dashboard::timeutil;

constexpr const char* kTag = "tasks";
constexpr const char* kNoData = "--";

/// How long a delete control stays armed after the first tap before reverting. Long enough to
/// read "Sure?" and tap again without hurrying, short enough that walking away leaves nothing
/// primed to delete by an incidental later tap.
constexpr uint32_t kDeleteConfirmWindowMs = 4000;

}  // namespace

TasksPlugin::TasksPlugin() : PluginBase("tasks", "To-dos") {}

uint32_t TasksPlugin::refreshIntervalMs() const {
    // "UI reconcile only" — the constant's own doc comment. The actual network side runs
    // continuously on TelegramService's own task and pokes notifyChanged() directly; this
    // interval only bounds how stale the page can get if that callback is ever missed.
    return dash::cfg::kTodosRefreshMs;
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void TasksPlugin::buildBody(lv_obj_t* body) {
    list_view_ = lv_obj_create(body);
    theme::makePlain(list_view_);
    lv_obj_set_width(list_view_, LV_PCT(100));
    lv_obj_set_flex_grow(list_view_, 1);
    lv_obj_set_flex_flow(list_view_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_view_, theme::kGapS, LV_PART_MAIN);

    for (size_t i = 0; i < kMaxRows; ++i) {
        TaskRow& row = rows_[i];
        row.root = theme::makeTapCard(list_view_);
        lv_obj_set_width(row.root, LV_PCT(100));
        lv_obj_set_height(row.root, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row.root, theme::kGapM, LV_PART_MAIN);

        // Tapping the row itself (title area) completes it — the row's own tap card handler,
        // not the delete button's. Both live on the same card, so the delete button is given its
        // own CLICKABLE child that stops the row's tap reaching it — see its handler below.
        lv_obj_add_event_cb(
            row.root,
            [](lv_event_t* event) {
                auto* self = static_cast<TasksPlugin*>(lv_event_get_user_data(event));
                auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
                const size_t index = reinterpret_cast<size_t>(lv_obj_get_user_data(target));
                self->onRowTapped(index);
            },
            LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(row.root, reinterpret_cast<void*>(i));

        row.title = theme::makeLabel(row.root, "", theme::fontBody(), theme::textPrimary());
        lv_obj_set_flex_grow(row.title, 1);
        lv_label_set_long_mode(row.title, LV_LABEL_LONG_DOT);

        // A small button, not part of the row's own click target: LVGL delivers CLICKED to the
        // deepest object under the finger, so a tap here fires ONLY this handler, never the
        // row's — no extra bubbling suppression needed.
        row.delete_btn = lv_button_create(row.root);
        lv_obj_remove_style_all(row.delete_btn);
        lv_obj_set_size(row.delete_btn, 72, theme::kTouchTarget - theme::kGapM);
        lv_obj_set_style_radius(row.delete_btn, theme::kRadius, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row.delete_btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row.delete_btn, theme::surfaceAlt(), LV_PART_MAIN);
        lv_obj_add_event_cb(
            row.delete_btn,
            [](lv_event_t* event) {
                auto* self = static_cast<TasksPlugin*>(lv_event_get_user_data(event));
                auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
                const size_t index = reinterpret_cast<size_t>(lv_obj_get_user_data(target));
                self->onDeleteTapped(index);
            },
            LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(row.delete_btn, reinterpret_cast<void*>(i));

        row.delete_label =
            theme::makeLabel(row.delete_btn, LV_SYMBOL_TRASH, theme::fontLabel(),
                             theme::textMuted());
        lv_obj_center(row.delete_label);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
    }

    empty_label_ =
        theme::makeLabel(list_view_, "Nothing to do. Text the bot to add something.",
                         theme::fontBody(), theme::textMuted());
    lv_obj_add_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t TasksPlugin::fetch(bool force) {
    (void)force;
    if (store_ == nullptr) {
        setError("no task store");
        return ESP_ERR_INVALID_STATE;
    }

    // Written STRAIGHT into the member array under the lock — no on-stack Task[kMaxRows]
    // intermediate. A Task is ~280 bytes (TextString title alone is 192), so a 10-entry stack
    // copy here would have spent most of this plugin's deliberately small worker stack on a copy
    // that gains nothing: nothing else needs the un-locked value.
    std::lock_guard<std::mutex> lock(modelMutex());
    rows_count_ = store_->snapshot(rows_snapshot_, kMaxRows, /*open_only=*/true);
    total_open_ = store_->openCount();
    markDirty();
    return ESP_OK;
}

void TasksPlugin::updateUi() {
    // Same reasoning as fetch(): render directly against the locked member array rather than
    // copying it onto the LVGL thread's stack first. renderList() only reads and touches no
    // mutex of its own, so holding modelMutex() for its duration cannot deadlock or block the
    // worker thread for any meaningful time.
    std::lock_guard<std::mutex> lock(modelMutex());
    renderList(rows_snapshot_, rows_count_, total_open_);
}

void TasksPlugin::onTick() {
    // Disarm a pending delete once its confirm window has passed — see kDeleteConfirmWindowMs.
    // Cheap even when nothing is armed: one lv_tick_elaps() call, no widget touched unless a
    // revert is actually due.
    if (!armed_delete_id_.empty() &&
        lv_tick_elaps(armed_delete_tick_) >= kDeleteConfirmWindowMs) {
        armed_delete_id_.clear();
        for (size_t i = 0; i < kMaxRows; ++i) {
            if (rows_[i].delete_label != nullptr) {
                lv_label_set_text(rows_[i].delete_label, LV_SYMBOL_TRASH);
                lv_obj_set_style_text_color(rows_[i].delete_label, theme::textMuted(),
                                            LV_PART_MAIN);
            }
        }
    }
}

void TasksPlugin::renderList(const dashboard::storage::Task* rows, size_t count,
                             size_t total_open) {
    if (empty_label_ == nullptr) {
        return;
    }

    if (count == 0) {
        lv_obj_remove_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < kMaxRows; ++i) {
        TaskRow& row = rows_[i];
        if (row.root == nullptr) {
            continue;
        }
        if (i >= count) {
            lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            row.task_id.clear();
            continue;
        }
        lv_obj_remove_flag(row.root, LV_OBJ_FLAG_HIDDEN);
        row.task_id.assign(rows[i].id.c_str());
        lv_label_set_text(row.title, rows[i].title.c_str());
    }

    // A row count beyond kMaxRows is not shown at all today — no "N more" line, unlike the
    // Telegram /list reply. Acceptable for now: kMaxTasks is 200 and this is a single screen's
    // worth of rows; revisit if a real user routinely has more than kMaxRows open at once.
    (void)total_open;
}

void TasksPlugin::onRowTapped(size_t row_index) {
    if (row_index >= kMaxRows || store_ == nullptr) {
        return;
    }
    // Copy the id out before posting: rows_[] can be re-rendered (and task_id reassigned) before
    // the posted job runs on the worker thread.
    dashboard::FixedString<24> id = rows_[row_index].task_id;
    if (id.empty()) {
        return;
    }

    TasksPlugin* self = this;
    dashboard::storage::TaskStore* store = store_;
    postToWorker([self, store, id]() {
        const std::time_t now = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;
        const esp_err_t err = store->complete(id.c_str(), now);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "complete('%s') failed: %s", id.c_str(), esp_err_to_name(err));
        }
        self->notifyChanged();
    });
}

void TasksPlugin::onDeleteTapped(size_t row_index) {
    if (row_index >= kMaxRows) {
        return;
    }
    TaskRow& row = rows_[row_index];
    if (row.task_id.empty()) {
        return;
    }

    if (armed_delete_id_.empty() || !armed_delete_id_.equals(row.task_id.c_str())) {
        // First tap on this row (or a different row was armed): arm THIS one, disarm any other.
        for (size_t i = 0; i < kMaxRows; ++i) {
            if (rows_[i].delete_label != nullptr) {
                const bool this_row = (i == row_index);
                lv_label_set_text(rows_[i].delete_label, this_row ? "Sure?" : LV_SYMBOL_TRASH);
                lv_obj_set_style_text_color(
                    rows_[i].delete_label, this_row ? theme::error() : theme::textMuted(),
                    LV_PART_MAIN);
            }
        }
        armed_delete_id_.assign(row.task_id.c_str());
        armed_delete_tick_ = lv_tick_get();
        return;
    }

    // Second tap within the window: actually delete.
    armed_delete_id_.clear();
    if (store_ == nullptr) {
        return;
    }
    dashboard::FixedString<24> id = row.task_id;
    TasksPlugin* self = this;
    dashboard::storage::TaskStore* store = store_;
    postToWorker([self, store, id]() {
        const esp_err_t err = store->remove(id.c_str());
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "remove('%s') failed: %s", id.c_str(), esp_err_to_name(err));
        }
        self->notifyChanged();
    });
}

void TasksPlugin::summarise(dashboard::PluginSummary& out) const {
    size_t count = 0;
    size_t total_open = 0;
    dashboard::storage::Task first;
    bool have_first = false;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        count = rows_count_;
        total_open = total_open_;
        if (count > 0) {
            first = rows_snapshot_[0];
            have_first = true;
        }
    }

    if (total_open == 0) {
        out.primary.assign("0");
        out.secondary.assign("All clear");
        out.tone = dashboard::SummaryTone::Good;
        return;
    }

    char count_text[16];
    std::snprintf(count_text, sizeof(count_text), "%u", static_cast<unsigned>(total_open));
    out.primary.assign(count_text);
    out.secondary.assign(have_first ? first.title.c_str() : kNoData);
    out.tone = dashboard::SummaryTone::Neutral;
}

}  // namespace plugins
