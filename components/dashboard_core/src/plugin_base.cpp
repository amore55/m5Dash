#include "dashboard/plugin_base.hpp"

#include <cstdio>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace dashboard {
namespace {

constexpr const char* kTag = "plugin";

/// Footer text is refreshed on this cadence even when the model has not changed, because
/// "Updated 3 min ago" ages on its own. Four ticks at 250 ms = once per second.
constexpr uint32_t kFooterRefreshEveryTicks = 4;

}  // namespace

PluginBase::PluginBase(const char* id, const char* title) : id_(id), title_(title) {}

PluginBase::~PluginBase() {
    // Stop the worker before anything else is torn down: a job in flight may still be touching
    // subclass members, and Worker::stop() waits for it to finish.
    worker_.stop();
}

esp_err_t PluginBase::initialise() {
    esp_err_t err = worker_.start(id_, workerStackBytes());
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "%s: worker failed to start (%s)", id_, esp_err_to_name(err));
        disable("internal error");
        return err;
    }

    err = onInitialise();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "%s: disabled — onInitialise returned %s", id_, esp_err_to_name(err));
        // onInitialise() is expected to have called disable() with a specific reason; supply a
        // generic one if it did not, so the page is never blank and unexplained.
        if (state() != DataState::Disabled) {
            disable("not configured");
        }
        return err;
    }

    ESP_LOGI(kTag, "%s: initialised", id_);
    return ESP_OK;
}

void PluginBase::createPage(lv_obj_t* parent) {
    ui_.build(parent, title_);
    buildBody(ui_.body());
    ui_.setState(state());
    refreshFooter();
}

void PluginBase::onShow() {
    visible_ = true;
    // Repaint immediately rather than waiting for the next tick, so a swipe never lands on a
    // page showing values from the last time it was visible.
    markDirty();
}

void PluginBase::onHide() { visible_ = false; }

void PluginBase::refresh(bool force) {
    // Called on the LVGL thread. Everything here must be O(1) and non-blocking.
    if (state() == DataState::Disabled || !worker_.running()) {
        return;
    }

    if (requiresNetwork() && !online()) {
        // No point attempting a connection. Downgrade to Stale if there is anything cached, so
        // the user sees "this is old" rather than a spinner that will never resolve.
        if (lastSuccessUtc() > 0 && state() == DataState::Ok) {
            setState(DataState::Stale);
            setError("offline");
            markDirty();
        } else if (lastSuccessUtc() == 0 && state() != DataState::Error) {
            setState(DataState::Error);
            setError("offline");
            markDirty();
        }
        return;
    }

    // Coalesce: a fetch is already running or queued. A forced refresh does not jump the queue
    // either, because the in-flight fetch will produce fresh data anyway.
    bool expected = false;
    if (!fetch_in_flight_.compare_exchange_strong(expected, true)) {
        return;
    }

    // Only show a spinner when there is nothing to show. If cached data is on screen we leave
    // it visible and refresh underneath, which is far less jarring on a glanceable display.
    if (lastSuccessUtc() == 0) {
        setState(DataState::Loading);
        markDirty();
    }

    if (!worker_.post([this, force]() { runFetch(force); })) {
        fetch_in_flight_.store(false, std::memory_order_release);
    }
}

void PluginBase::runFetch(bool force) {
    // WORKER THREAD.
    const esp_err_t err = fetch(force);

    if (err == ESP_OK) {
        last_success_.store(static_cast<long long>(timeutil::nowUtc()),
                            std::memory_order_relaxed);
        setState(DataState::Ok);
        setError(nullptr);
    } else {
        ESP_LOGW(kTag, "%s: fetch failed: %s", id_, esp_err_to_name(err));
        // Having *something* to show is the difference between Stale and Error. This is why
        // plugins cache: a failed refresh degrades rather than blanking the page.
        setState(lastSuccessUtc() > 0 ? DataState::Stale : DataState::Error);
        // fetch() is expected to have called setError(); fall back to the error code so the
        // footer is never silently empty.
        {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            if (detail_.empty()) {
                detail_.assign(esp_err_to_name(err));
            }
        }
    }

    fetch_in_flight_.store(false, std::memory_order_release);
    markDirty();
}

void PluginBase::tick() {
    // LVGL THREAD, every dash::cfg::kUiTickPeriodMs. Keep it cheap: this runs for every
    // registered plugin on every tick, visible or not.
    ++tick_counter_;

    applyStaleIfAged();

    const bool periodic = (tick_counter_ % kFooterRefreshEveryTicks == 0);

    if (dirty_.exchange(false, std::memory_order_acq_rel)) {
        if (ui_.built()) {
            updateUi();
            ui_.setState(state());
            ui_.setOffline(requiresNetwork() && !online());
            refreshFooter();
        }
    } else if (ui_.built() && periodic) {
        // The relative-age text ages even when nothing else changed.
        refreshFooter();
    }

    if (ui_.built() && periodic) {
        updateHeaderClock();
    }

    onTick();
}

void PluginBase::onNetworkChanged(bool online_now) {
    online_.store(online_now, std::memory_order_relaxed);
    markDirty();
    if (online_now && requiresNetwork()) {
        // Come back promptly rather than waiting out the remainder of the interval — the whole
        // point of observing connectivity is not making the user stare at stale data.
        refresh(false);
    }
}

void PluginBase::setState(DataState state) {
    state_.store(state, std::memory_order_relaxed);
}

void PluginBase::setError(const char* reason) {
    std::lock_guard<std::mutex> lock(detail_mutex_);
    detail_.assign(reason);
}

void PluginBase::disable(const char* reason) {
    setState(DataState::Disabled);
    setError(reason);
    markDirty();
}

void PluginBase::applyStaleIfAged() {
    if (state() != DataState::Ok) {
        return;
    }
    const long long last = last_success_.load(std::memory_order_relaxed);
    if (last <= 0) {
        return;
    }
    const uint32_t interval_ms = refreshIntervalMs();
    if (interval_ms == 0) {
        return;
    }
    // Data is stale once it is older than a few of its own refresh intervals: that means
    // several attempts have silently failed to land, even if none reported an error.
    const long long limit_seconds =
        static_cast<long long>(interval_ms / 1000) * dash::cfg::kStaleAfterIntervals;
    if (static_cast<long long>(timeutil::nowUtc()) - last > limit_seconds) {
        setState(DataState::Stale);
        markDirty();
    }
}

void PluginBase::updateHeaderClock() {
    if (!showHeaderClock()) {
        return;
    }
    if (!timeutil::systemTimeValid()) {
        // "--:--" rather than an empty header: an absent clock reads as a layout bug, whereas
        // dashes clearly say "the time is not known yet".
        ui_.setHeaderClock("--:--");
        return;
    }
    const std::tm now = timeutil::localNow();
    char text[8];
    timeutil::formatTime24h(text, sizeof(text), now, /*with_seconds=*/false);
    ui_.setHeaderClock(text);
}

void PluginBase::refreshFooter() {
    if (!ui_.built()) {
        return;
    }

    char detail[96];
    {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        std::snprintf(detail, sizeof(detail), "%s", detail_.c_str());
    }

    const DataState current = state();
    const std::time_t last = lastSuccessUtc();
    char line[160];
    lv_color_t colour = theme::textMuted();

    switch (current) {
        // NOTE: footer strings must stay ASCII. LVGL's built-in Montserrat faces only carry
        // ASCII plus a handful of LV_SYMBOL glyphs, so an em dash or an ellipsis renders as an
        // empty box rather than the character intended.
        case DataState::Disabled:
            std::snprintf(line, sizeof(line), "Disabled%s%s", detail[0] != '\0' ? " - " : "",
                          detail);
            colour = theme::textMuted();
            break;

        case DataState::Loading:
            std::snprintf(line, sizeof(line), "Updating...");
            colour = theme::accent();
            break;

        case DataState::Ok: {
            char age[32];
            timeutil::formatRelativeAge(age, sizeof(age), last, timeutil::nowUtc());
            std::snprintf(line, sizeof(line), "Updated %s", age);
            colour = theme::textMuted();
            break;
        }

        case DataState::Stale: {
            // Say both things: how old the data is, and why it did not refresh. Either alone
            // leaves the user guessing.
            char age[32];
            timeutil::formatRelativeAge(age, sizeof(age), last, timeutil::nowUtc());
            if (detail[0] != '\0') {
                std::snprintf(line, sizeof(line), "Data from %s - %s", age, detail);
            } else {
                std::snprintf(line, sizeof(line), "Data from %s - refresh failed", age);
            }
            colour = theme::stale();
            break;
        }

        case DataState::Error:
            std::snprintf(line, sizeof(line), "Unavailable%s%s", detail[0] != '\0' ? " - " : "",
                          detail);
            colour = theme::error();
            break;

        case DataState::Idle:
            line[0] = '\0';
            break;
    }

    ui_.setFooterColour(colour);
    ui_.setFooterText(line);
}

}  // namespace dashboard
