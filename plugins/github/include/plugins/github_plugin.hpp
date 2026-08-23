// Page: GitHub.
//
// TWO VIEWS IN ONE PAGE, not two pages. The list of repositories and the drill-down into one
// repository's runs are the same page swapping which of two containers is visible, because
// PageManager's navigation is a flat rotation — a second registered page would appear in the
// swipe order and in Settings as something to enable, which is not what a drill-down is.
//
// Back out of the drill-down with the Back button on it. The home icon still goes to the summary
// page from either view.
//
// THE REFRESH IS SLOW AND THAT SHAPES THE PAGE. There is no endpoint for "latest run of each of
// my repositories", so a refresh is one request for the list plus one per repository — eleven,
// each a fresh TLS handshake, serialised device-wide. That is around fifteen seconds. So:
//
//   * the repository list is published to the UI the moment it arrives, before any run states,
//   * each run state is published as its own request lands, so rows fill in visibly,
//   * rows say "checking" until asked, and then "no actions" if that is the answer.
//
// A page that waited for all eleven would show nothing for fifteen seconds and look broken.

#pragma once

#include <atomic>
#include <functional>
#include <cstddef>

#include "dashboard/plugin_base.hpp"
#include "plugins/github_model.hpp"
#include "plugins/github_provider.hpp"

namespace plugins {

class GithubPlugin final : public dashboard::PluginBase {
  public:
    GithubPlugin();

    uint32_t refreshIntervalMs() const override;

    /// Whose repositories to show, which organisation counts as work, and which list is up.
    /// Applied live from Settings; a change refetches.
    void setAccount(const char* username, const char* organisation, bool show_work);

    /// True when the WORK list is showing rather than the user's own.
    bool showingWork() const { return show_work_.load(std::memory_order_relaxed); }

    /// Called when the filter is changed FROM THE PAGE, so the choice can be written to settings
    /// and survive a restart.
    ///
    /// A callback rather than this plugin writing settings itself: the filter lives in two places
    /// (a button here, a stored value there) and only the application knows how to persist one.
    /// Without it the buttons worked and the choice silently reverted on every boot.
    void setFilterPersister(std::function<void(bool show_work)> persist) {
        persist_filter_ = std::move(persist);
    }

    /// The most recent run across the listed repositories, for the summary tile.
    void summarise(dashboard::PluginSummary& out) const override;

  protected:
    esp_err_t onInitialise() override;
    void buildBody(lv_obj_t* body) override;
    esp_err_t fetch(bool force) override;
    void updateUi() override;
    void onShow() override;

    /// Eleven sequential HTTPS requests and a cJSON parse of up to 112 KB. The response buffers
    /// are in PSRAM (ResponseBuffer), so this covers only cJSON's recursion over a deep document
    /// — GitHub run objects nest repository inside run inside array.
    uint32_t workerStackBytes() const override { return 12288; }

  private:
    /// Rows in the repository list. Ten, matching RepoList::kMaxRepos.
    static constexpr size_t kMaxRows = RepoList::kMaxRepos;

    // ---- layout -------------------------------------------------------------------------
    void buildListView(lv_obj_t* parent);
    void buildDetailView(lv_obj_t* parent);
    void showDetail(size_t row_index);
    void showList();

    void renderList(const RepoList& repos, std::time_t now_utc);
    void renderDetail(const RunList& runs, std::time_t now_utc);
    void updateFilterButtons();

    // ---- data ---------------------------------------------------------------------------
    /// Fetch the repository list for `scope`, then each repository's latest run, publishing as
    /// it goes.
    ///
    /// `primary` distinguishes the list the user is looking at from the one being warmed behind
    /// it. Only a primary failure reaches the footer: reporting "no work token stored" on a page
    /// showing personal repositories would be an error about something nobody asked for.
    esp_err_t refreshRepositories(RepoScope scope, bool primary);

    /// Fetch the runs for whichever repository the drill-down is showing.
    esp_err_t refreshDetail();

    /// The list for the scope currently on screen. Caller must hold modelMutex().
    RepoList& activeList();
    const RepoList& activeList() const;

    GithubProvider provider_;

    /// Guarded by modelMutex().
    ///
    /// BOTH lists are kept, not just the visible one, so pressing the other filter shows
    /// something immediately instead of twelve seconds of nothing. The newly-selected list is
    /// refetched behind whatever was cached.
    RepoList mine_;
    RepoList work_;
    RunList detail_runs_;

    /// Which scope the drill-down belongs to, so its runs are fetched with the right token — the
    /// personal token cannot see a work repository at all.
    RepoScope detail_scope_ = RepoScope::Mine;

    /// Which repository the drill-down is for. Set on the LVGL thread by a row press, read on
    /// the worker. A FixedString rather than an index because the list can be refetched and
    /// reordered between the press and the fetch landing.
    dashboard::MediumString detail_full_name_;
    std::atomic<bool> detail_pending_{false};

    dashboard::ShortString username_{"amore55"};
    dashboard::ShortString organisation_;
    std::atomic<bool> show_work_{false};

    /// Set by the application; see setFilterPersister(). LVGL thread only.
    std::function<void(bool)> persist_filter_;

    /// Set when setAccount() changes something, so fetch() knows to discard what it holds
    /// rather than merging new repositories into an old list.
    std::atomic<bool> account_changed_{false};

    // ---- widgets ------------------------------------------------------------------------
    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* detail_view_ = nullptr;

    lv_obj_t* work_button_ = nullptr;
    lv_obj_t* mine_button_ = nullptr;
    lv_obj_t* list_empty_ = nullptr;

    struct RepoRow {
        lv_obj_t* root = nullptr;
        lv_obj_t* name = nullptr;
        lv_obj_t* actor = nullptr;
        lv_obj_t* status = nullptr;
        lv_obj_t* workflow = nullptr;
        lv_obj_t* age = nullptr;
        /// Which repository this row currently shows, so the click handler does not depend on an
        /// index into a list that may have been re-sorted since.
        dashboard::MediumString full_name;
    };
    RepoRow rows_[kMaxRows];

    lv_obj_t* detail_title_ = nullptr;
    lv_obj_t* detail_empty_ = nullptr;
    struct RunRow {
        lv_obj_t* root = nullptr;
        lv_obj_t* number = nullptr;
        lv_obj_t* title = nullptr;
        lv_obj_t* actor = nullptr;
        lv_obj_t* status = nullptr;
        lv_obj_t* age = nullptr;
    };
    RunRow run_rows_[RunList::kMaxRuns];
};

}  // namespace plugins
