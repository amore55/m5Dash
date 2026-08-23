// Base class implementing the plugin threading contract, so no plugin has to re-derive it.
//
// A subclass provides four things and gets correct behaviour for free:
//
//   buildBody(body)   LVGL thread, once   — create your widgets
//   fetch(force)      WORKER thread       — do the network / parsing / disk work
//   updateUi()        LVGL thread         — copy your model into your widgets
//   onTick()          LVGL thread, 250 ms — optional; clocks and countdowns use it
//
// PluginBase handles: worker lifecycle, the Loading/Ok/Stale/Error state machine, the
// automatic transition to Stale when data ages out, the footer's "Updated N min ago" line, the
// header status dot, offline suppression, and the dirty-flag handoff between threads.
//
// MUTEX DISCIPLINE — two separate mutexes on purpose:
//
//   modelMutex()  is YOURS. Guard your own parsed model with it. fetch() writes under it;
//                 updateUi() reads under it.
//   status/detail are guarded internally and reached only via setState()/setError(), which are
//                 safe to call from either thread.
//
// They are separate so that calling setError() while holding modelMutex() — the obvious thing
// to do inside a fetch() failure path — cannot deadlock.

#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>

#include "dashboard/fixed_string.hpp"
#include "dashboard/page_scaffold.hpp"
#include "dashboard/plugin.hpp"
#include "dashboard/worker.hpp"

namespace dashboard {

class PluginBase : public DashboardPlugin {
  public:
    /// `id` and `title` must be string literals with static lifetime — they are stored by
    /// pointer, not copied, and `id` is also used verbatim as the worker task name.
    PluginBase(const char* id, const char* title);
    ~PluginBase() override;

    const char* id() const final { return id_; }
    const char* title() const final { return title_; }

    esp_err_t initialise() final;
    void createPage(lv_obj_t* parent) final;
    void onShow() override;
    void onHide() override;
    void refresh(bool force) final;
    void tick() final;
    void onNetworkChanged(bool online) override;
    DataState state() const final { return state_.load(std::memory_order_relaxed); }

  protected:
    // ---------------------------------------------------------------------------------
    // Implement these
    // ---------------------------------------------------------------------------------

    /// Optional one-time setup: open storage, load a cached response, validate configuration.
    /// Runs on the caller's thread (app start-up), before createPage().
    /// Return anything but ESP_OK to disable the plugin — the dashboard carries on without it.
    virtual esp_err_t onInitialise() { return ESP_OK; }

    /// Build widgets inside the scaffold's body. LVGL thread, called exactly once.
    virtual void buildBody(lv_obj_t* body) = 0;

    /// Do the actual work. **WORKER THREAD — must not touch any lv_obj_t.**
    /// On failure, call setError() with a short user-facing reason and return an esp_err_t.
    /// Successful returns update the last-success timestamp automatically.
    virtual esp_err_t fetch(bool force) = 0;

    /// Copy the model into widgets. LVGL thread. Called when markDirty() has been signalled.
    virtual void updateUi() = 0;

    /// Optional per-tick work on the LVGL thread (a ticking clock, a countdown).
    /// Runs every dash::cfg::kUiTickPeriodMs whether or not the page is visible.
    virtual void onTick() {}

    /// Worker stack size. The default accommodates a TLS handshake plus JSON parsing; a plugin
    /// doing no networking can safely reduce it.
    virtual uint32_t workerStackBytes() const { return 8192; }

    /// If true, refresh() is suppressed while offline (and existing data marked Stale) rather
    /// than burning a doomed connection attempt. The clock plugin overrides this to false.
    virtual bool requiresNetwork() const { return true; }

    /// Whether to show the small HH:MM clock in the page header.
    ///
    /// PluginBase renders it rather than PageManager, because PageManager only holds a
    /// DashboardPlugin* and cannot reach a plugin's scaffold. The clock plugin overrides this
    /// to false — a second, smaller clock on the clock page would be absurd.
    virtual bool showHeaderClock() const { return true; }

    // ---------------------------------------------------------------------------------
    // Use these
    // ---------------------------------------------------------------------------------

    /// Signal that updateUi() should run on the next tick. Safe from any thread.
    void markDirty() { dirty_.store(true, std::memory_order_release); }

    /// Safe from any thread.
    void setState(DataState state);

    /// Record a short, user-facing failure reason shown in the footer.
    /// MUST NOT contain a credential — it is rendered on screen and may be logged.
    void setError(const char* reason);

    /// Guard for the subclass's own model. See the mutex discipline note above.
    ///
    /// const, returning a non-const reference, because locking is not a modification of the
    /// plugin's logical state: a read-only accessor like summarise() still has to lock against
    /// the worker thread. Hence the mutable member.
    std::mutex& modelMutex() const { return model_mutex_; }

    PageScaffold& ui() { return ui_; }
    bool visible() const { return visible_; }
    bool online() const { return online_.load(std::memory_order_relaxed); }
    std::time_t lastSuccessUtc() const {
        return static_cast<std::time_t>(last_success_.load(std::memory_order_relaxed));
    }

    /// Mark the plugin unusable (missing token, unsupported configuration). Sticky: a disabled
    /// plugin is never scheduled for refresh. Call from onInitialise().
    void disable(const char* reason);

    /// Declare that data from a PREVIOUS run is now on screen, stamped with when it was fetched.
    /// For a plugin that loads a cached response in onInitialise().
    ///
    /// Without this the state machine believes the plugin has never held data, and the two places
    /// that check `lastSuccessUtc() > 0` both get it wrong: a failed first refresh reports
    /// "Unavailable" while cached values are plainly visible, and the footer never says how old
    /// what you are looking at is. Moves the plugin to Stale, which is exactly what it is.
    ///
    /// Ignores a zero or older timestamp, so it can never walk the last-success time backwards.
    void noteCachedData(std::time_t fetched_utc);

    /// Post a job to the plugin's worker without going through the refresh state machine.
    /// For side tasks such as persisting a task list after a touch interaction.
    bool postToWorker(Worker::Job job) { return worker_.post(std::move(job)); }

    bool workerBusy() const { return worker_.busy(); }

  private:
    void runFetch(bool force);
    void refreshFooter();
    void updateHeaderClock();
    void applyStaleIfAged();

    const char* id_;
    const char* title_;

    PageScaffold ui_;
    Worker worker_;

    mutable std::mutex model_mutex_;

    std::mutex detail_mutex_;
    FixedString<96> detail_;

    std::atomic<DataState> state_{DataState::Idle};
    std::atomic<long long> last_success_{0};
    std::atomic<bool> dirty_{false};
    std::atomic<bool> online_{false};
    std::atomic<bool> fetch_in_flight_{false};

    bool visible_ = false;
    uint32_t tick_counter_ = 0;
};

}  // namespace dashboard
