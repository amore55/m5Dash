#include "plugins/github_plugin.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/theme.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace theme = dashboard::theme;
namespace timeutil = dashboard::timeutil;

constexpr const char* kTag = "github";

/// Column widths for the repository list. Fixed so the columns line up down the page, as on the
/// Elizabeth board — the same reasoning, and the same reason not to flex them.
constexpr int32_t kStatusColumn = 200;
constexpr int32_t kWorkflowColumn = 260;
constexpr int32_t kAgeColumn = 190;
constexpr int32_t kRowHeight = 64;

/// Drill-down columns.
constexpr int32_t kRunNumberColumn = 110;
constexpr int32_t kRunStatusColumn = 200;
constexpr int32_t kRunAgeColumn = 190;

constexpr const char* kNoData = "--";

/// Colour a run state. Amber for busy is deliberate: "running" is neither good news nor bad, and
/// the whole point of the page is spotting that something IS running.
lv_color_t colourForRunState(RunState state) {
    switch (state) {
        case RunState::Success:
            return theme::ok();
        case RunState::Failure:
            return theme::error();
        case RunState::Queued:
        case RunState::InProgress:
            return theme::stale();
        case RunState::Cancelled:
        case RunState::Skipped:
        case RunState::None:
        case RunState::Unknown:
        default:
            return theme::textMuted();
    }
}

}  // namespace

GithubPlugin::GithubPlugin() : PluginBase("github", "GitHub") {}

uint32_t GithubPlugin::refreshIntervalMs() const {
    // Deliberately slow. A refresh is eleven requests; unauthenticated GitHub allows sixty an
    // hour, so anything much faster than this cannot complete twice without being rate limited.
    return dash::cfg::kGithubRefreshMs;
}

void GithubPlugin::setAccount(const char* username, bool all_repositories) {
    bool changed = false;
    if (username != nullptr && username[0] != '\0' && !username_.equals(username)) {
        username_.assign(username);
        changed = true;
    }
    if (all_repositories_.exchange(all_repositories, std::memory_order_relaxed) !=
        all_repositories) {
        changed = true;
    }
    if (changed) {
        account_changed_.store(true, std::memory_order_relaxed);
        refresh(/*force=*/true);
    }
}

esp_err_t GithubPlugin::onInitialise() {
    if (!GithubProvider::authenticated()) {
        ESP_LOGW(kTag, "no token stored: public repositories only, 60 requests/hour");
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------

void GithubPlugin::buildBody(lv_obj_t* body) {
    // Both views are children of the body and only one is visible. See the header for why this
    // is not two registered pages.
    buildListView(body);
    buildDetailView(body);
    showList();
}

void GithubPlugin::buildListView(lv_obj_t* parent) {
    list_view_ = lv_obj_create(parent);
    theme::makePlain(list_view_);
    lv_obj_set_width(list_view_, LV_PCT(100));
    lv_obj_set_flex_grow(list_view_, 1);
    lv_obj_set_flex_flow(list_view_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_view_, theme::kGapS, LV_PART_MAIN);

    // Filter row.
    lv_obj_t* filters = theme::makeRow(list_view_);
    lv_obj_set_height(filters, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(filters, theme::kGapS, LV_PART_MAIN);

    all_button_ = theme::makeButton(filters, "All repositories");
    lv_obj_add_event_cb(
        all_button_,
        [](lv_event_t* event) {
            auto* self = static_cast<GithubPlugin*>(lv_event_get_user_data(event));
            self->setAccount(nullptr, /*all_repositories=*/true);
            self->updateFilterButtons();
        },
        LV_EVENT_CLICKED, this);

    mine_button_ = theme::makeButton(filters, "My repositories");
    lv_obj_add_event_cb(
        mine_button_,
        [](lv_event_t* event) {
            auto* self = static_cast<GithubPlugin*>(lv_event_get_user_data(event));
            self->setAccount(nullptr, /*all_repositories=*/false);
            self->updateFilterButtons();
        },
        LV_EVENT_CLICKED, this);
    updateFilterButtons();

    theme::makeSeparator(list_view_);

    for (size_t i = 0; i < kMaxRows; ++i) {
        RepoRow& row = rows_[i];
        // A tap card rather than a plain row: every row drills down, so every row is a button and
        // should look and feel like one.
        row.root = theme::makeTapCard(list_view_);
        lv_obj_set_width(row.root, LV_PCT(100));
        lv_obj_set_height(row.root, kRowHeight);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_ver(row.root, theme::kGapS, LV_PART_MAIN);
        lv_obj_set_style_pad_column(row.root, theme::kGapM, LV_PART_MAIN);

        // The row index rides on the widget. renderList() keeps rows_[i].full_name in step, and
        // the handler reads that rather than trusting an index into the model.
        lv_obj_set_user_data(row.root, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(
            row.root,
            [](lv_event_t* event) {
                auto* self = static_cast<GithubPlugin*>(lv_event_get_user_data(event));
                auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
                const auto index =
                    static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
                self->showDetail(index);
            },
            LV_EVENT_CLICKED, this);

        row.name = theme::makeLabel(row.root, "", theme::fontTitle(), theme::textPrimary());
        lv_obj_set_flex_grow(row.name, 1);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);

        row.status = theme::makeLabel(row.root, "", theme::fontBody(), theme::textMuted());
        lv_obj_set_width(row.status, kStatusColumn);

        row.workflow = theme::makeLabel(row.root, "", theme::fontBody(), theme::textMuted());
        lv_obj_set_width(row.workflow, kWorkflowColumn);
        lv_label_set_long_mode(row.workflow, LV_LABEL_LONG_DOT);

        row.age = theme::makeLabel(row.root, "", theme::fontBody(), theme::textSecondary());
        lv_obj_set_width(row.age, kAgeColumn);
        lv_obj_set_style_text_align(row.age, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
    }

    list_empty_ = theme::makeLabel(list_view_, "", theme::fontBody(), theme::textMuted());
    lv_obj_add_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
}

void GithubPlugin::buildDetailView(lv_obj_t* parent) {
    detail_view_ = lv_obj_create(parent);
    theme::makePlain(detail_view_);
    lv_obj_set_width(detail_view_, LV_PCT(100));
    lv_obj_set_flex_grow(detail_view_, 1);
    lv_obj_set_flex_flow(detail_view_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(detail_view_, theme::kGapS, LV_PART_MAIN);

    lv_obj_t* header = theme::makeRow(detail_view_);
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(header, theme::kGapM, LV_PART_MAIN);

    lv_obj_t* back = theme::makeButton(header, LV_SYMBOL_LEFT " Back");
    lv_obj_add_event_cb(
        back,
        [](lv_event_t* event) {
            static_cast<GithubPlugin*>(lv_event_get_user_data(event))->showList();
        },
        LV_EVENT_CLICKED, this);

    detail_title_ = theme::makeLabel(header, "", theme::fontTitle(), theme::textPrimary());
    lv_obj_set_flex_grow(detail_title_, 1);
    lv_label_set_long_mode(detail_title_, LV_LABEL_LONG_DOT);

    theme::makeSeparator(detail_view_);

    for (size_t i = 0; i < RunList::kMaxRuns; ++i) {
        RunRow& row = run_rows_[i];
        row.root = theme::makeRow(detail_view_);
        lv_obj_set_height(row.root, kRowHeight);
        lv_obj_set_style_pad_column(row.root, theme::kGapM, LV_PART_MAIN);

        row.number = theme::makeLabel(row.root, "", theme::fontBody(), theme::textMuted());
        lv_obj_set_width(row.number, kRunNumberColumn);

        row.title = theme::makeLabel(row.root, "", theme::fontBody(), theme::textPrimary());
        lv_obj_set_flex_grow(row.title, 1);
        lv_label_set_long_mode(row.title, LV_LABEL_LONG_DOT);

        row.status = theme::makeLabel(row.root, "", theme::fontBody(), theme::textMuted());
        lv_obj_set_width(row.status, kRunStatusColumn);

        row.age = theme::makeLabel(row.root, "", theme::fontBody(), theme::textSecondary());
        lv_obj_set_width(row.age, kRunAgeColumn);
        lv_obj_set_style_text_align(row.age, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
    }

    detail_empty_ = theme::makeLabel(detail_view_, "", theme::fontBody(), theme::textMuted());
    lv_obj_add_flag(detail_empty_, LV_OBJ_FLAG_HIDDEN);
}

void GithubPlugin::updateFilterButtons() {
    const bool all = allRepositories();
    theme::setButtonSelected(all_button_, all);
    theme::setButtonSelected(mine_button_, !all);
}

void GithubPlugin::showList() {
    if (list_view_ == nullptr || detail_view_ == nullptr) {
        return;
    }
    lv_obj_remove_flag(list_view_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_view_, LV_OBJ_FLAG_HIDDEN);
    detail_pending_.store(false, std::memory_order_relaxed);
}

void GithubPlugin::showDetail(size_t row_index) {
    if (row_index >= kMaxRows || rows_[row_index].full_name.empty()) {
        return;  // a press on a row that is not currently showing anything
    }

    detail_full_name_ = rows_[row_index].full_name;
    lv_label_set_text(detail_title_, detail_full_name_.c_str());

    // Clear whatever the previous repository left behind, so there is never a moment where one
    // repository's title sits above another's runs.
    for (auto& row : run_rows_) {
        lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(detail_empty_, "Loading runs...");
    lv_obj_remove_flag(detail_empty_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(list_view_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(detail_view_, LV_OBJ_FLAG_HIDDEN);

    detail_pending_.store(true, std::memory_order_relaxed);
    refresh(/*force=*/true);
}

void GithubPlugin::onShow() {
    PluginBase::onShow();
    // Always arrive at the list. Coming back to the page and finding a drill-down from an hour
    // ago still on screen would be disorienting.
    showList();
}

// ---------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------

esp_err_t GithubPlugin::fetch(bool force) {
    (void)force;

    // A drill-down request jumps the queue: the user is looking at that view now, and making
    // them wait out a fifteen-second repository refresh first would feel broken.
    if (detail_pending_.exchange(false, std::memory_order_relaxed)) {
        return refreshDetail();
    }
    return refreshRepositories();
}

esp_err_t GithubPlugin::refreshDetail() {
    dashboard::MediumString target;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        target = detail_full_name_;
    }
    if (target.empty()) {
        return ESP_OK;
    }

    dashboard::net::ResponseBuffer buffer(GithubProvider::kRunsResponseBytes);
    if (!buffer.valid()) {
        setError("out of memory");
        return ESP_ERR_NO_MEM;
    }

    RunList runs;
    const esp_err_t err =
        provider_.fetchRuns(target.c_str(), buffer.data(), buffer.capacity(), runs);
    if (err != ESP_OK) {
        setError(provider_.lastError());
        return err;
    }

    {
        std::lock_guard<std::mutex> lock(modelMutex());
        detail_runs_ = runs;
    }
    markDirty();
    return ESP_OK;
}

esp_err_t GithubPlugin::refreshRepositories() {
    const bool all = all_repositories_.load(std::memory_order_relaxed);
    account_changed_.store(false, std::memory_order_relaxed);

    dashboard::ShortString username;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        username = username_;
    }

    RepoList repos;
    {
        dashboard::net::ResponseBuffer buffer(GithubProvider::kReposResponseBytes);
        if (!buffer.valid()) {
            setError("out of memory");
            return ESP_ERR_NO_MEM;
        }
        const esp_err_t err = provider_.fetchRepos(username.c_str(), all, buffer.data(),
                                                   buffer.capacity(), repos);
        if (err != ESP_OK) {
            setError(provider_.lastError());
            return err;
        }
    }

    // PUBLISH THE LIST NOW, before any run states. This is the whole reason the page feels
    // responsive: the names and push times are already useful, and the statuses take another
    // ten requests to arrive.
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        repos_ = repos;
    }
    markDirty();

    // Then one request per repository, publishing after each so rows fill in visibly.
    dashboard::net::ResponseBuffer buffer(GithubProvider::kRunsResponseBytes);
    if (!buffer.valid()) {
        return ESP_OK;  // the list stands; statuses are enrichment
    }

    size_t failures = 0;
    size_t with_runs = 0;
    for (size_t i = 0; i < repos.count; ++i) {
        // Abandon the remaining requests if the account changed or a drill-down was asked for
        // while this loop was running — ten more TLS handshakes for a list nobody is looking at
        // is exactly the kind of thing that makes a device feel slow.
        if (account_changed_.load(std::memory_order_relaxed) ||
            detail_pending_.load(std::memory_order_relaxed)) {
            ESP_LOGI(kTag, "abandoning run lookups: newer request pending");
            break;
        }

        RepoEntry entry = repos.repos[i];
        if (provider_.fetchLatestRun(entry.full_name.c_str(), buffer.data(), buffer.capacity(),
                                     entry) != ESP_OK) {
            ++failures;
            continue;  // leave run_known false: the row keeps saying "checking"
        }
        if (entry.run_state != RunState::None) {
            ++with_runs;
            ESP_LOGI(kTag, "%s: %s (%s)", entry.full_name.c_str(),
                     runStateText(entry.run_state),
                     entry.run_workflow.empty() ? "unnamed workflow"
                                                : entry.run_workflow.c_str());
        }

        std::lock_guard<std::mutex> lock(modelMutex());
        // Match by name, not by index: the list could have been replaced under us by a forced
        // refresh, and writing to slot i would then label the wrong repository.
        for (size_t j = 0; j < repos_.count; ++j) {
            if (repos_.repos[j].full_name == entry.full_name) {
                repos_.repos[j] = entry;
                break;
            }
        }
        markDirty();
    }

    ESP_LOGI(kTag, "run lookups: %u of %u repositories have workflow runs, %u failed",
             static_cast<unsigned>(with_runs), static_cast<unsigned>(repos.count),
             static_cast<unsigned>(failures));

    if (failures > 0 && failures == repos.count) {
        // Every status failed, which almost always means the rate limit. Worth saying, because
        // the list looks fine and the reason is invisible otherwise.
        setError(provider_.lastError());
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------

void GithubPlugin::updateUi() {
    RepoList repos;
    RunList runs;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        repos = repos_;
        runs = detail_runs_;
    }

    const std::time_t now = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;
    renderList(repos, now);
    renderDetail(runs, now);
    updateFilterButtons();
}

void GithubPlugin::renderList(const RepoList& repos, std::time_t now_utc) {
    if (list_empty_ == nullptr) {
        return;
    }

    if (repos.valid && repos.count == 0) {
        lv_label_set_text(list_empty_,
                          GithubProvider::authenticated()
                              ? "No repositories found for this account."
                              : "No public repositories. Add a token in settings to see private "
                                "and organisation repositories.");
        lv_obj_remove_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(list_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < kMaxRows; ++i) {
        RepoRow& row = rows_[i];
        if (i >= repos.count) {
            lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            row.full_name.clear();
            continue;
        }

        const RepoEntry& repo = repos.repos[i];
        row.full_name = repo.full_name;
        lv_obj_remove_flag(row.root, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(row.name, repo.name.c_str());

        if (!repo.run_known) {
            // Asked-for but not yet answered. Distinct from "no actions", which is an answer.
            lv_label_set_text(row.status, "Checking...");
            lv_obj_set_style_text_color(row.status, theme::textMuted(), LV_PART_MAIN);
            lv_label_set_text(row.workflow, "");
        } else {
            lv_label_set_text(row.status, runStateText(repo.run_state));
            lv_obj_set_style_text_color(row.status, colourForRunState(repo.run_state),
                                        LV_PART_MAIN);
            lv_label_set_text(row.workflow, repo.run_workflow.c_str());
        }

        // The age shown is the last PUSH, not the last run: that is what "last worked on" means,
        // and a repository with no Actions still has a meaningful one.
        char age[32];
        formatRelativeAge(age, sizeof(age), repo.pushed_utc, now_utc);
        lv_label_set_text(row.age, age[0] != '\0' ? age : kNoData);
    }
}

void GithubPlugin::renderDetail(const RunList& runs, std::time_t now_utc) {
    if (detail_empty_ == nullptr) {
        return;
    }

    // Ignore a response for a repository the user has since navigated away from.
    if (runs.valid && !detail_full_name_.empty() && !(runs.full_name == detail_full_name_)) {
        return;
    }

    if (runs.valid && runs.count == 0) {
        lv_label_set_text(detail_empty_, "This repository has no workflow runs.");
        lv_obj_remove_flag(detail_empty_, LV_OBJ_FLAG_HIDDEN);
    } else if (runs.valid) {
        lv_obj_add_flag(detail_empty_, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = 0; i < RunList::kMaxRuns; ++i) {
        RunRow& row = run_rows_[i];
        if (i >= runs.count) {
            lv_obj_add_flag(row.root, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const RunEntry& run = runs.runs[i];
        lv_obj_remove_flag(row.root, LV_OBJ_FLAG_HIDDEN);

        char number[24];
        std::snprintf(number, sizeof(number), "#%lld", static_cast<long long>(run.number));
        lv_label_set_text(row.number, number);

        lv_label_set_text(row.title,
                          run.title.empty() ? run.workflow.c_str() : run.title.c_str());

        lv_label_set_text(row.status, runStateText(run.state));
        lv_obj_set_style_text_color(row.status, colourForRunState(run.state), LV_PART_MAIN);

        char age[32];
        formatRelativeAge(age, sizeof(age), run.updated_utc, now_utc);
        lv_label_set_text(row.age, age[0] != '\0' ? age : kNoData);
    }
}

void GithubPlugin::summarise(dashboard::PluginSummary& out) const {
    RepoList repos;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        repos = repos_;
    }

    if (!repos.valid || repos.count == 0) {
        out.primary.assign(kNoData);
        out.secondary.assign(GithubProvider::authenticated() ? "No repositories"
                                                             : "Add a token in settings");
        return;
    }

    // The tile answers "what is happening with my code right now", so a RUNNING job outranks a
    // more recent finished one — that is the thing you would want to know without opening the
    // page. Otherwise fall back to the most recently pushed repository.
    const RepoEntry* chosen = nullptr;
    for (size_t i = 0; i < repos.count; ++i) {
        if (repos.repos[i].run_known && runStateBusy(repos.repos[i].run_state)) {
            chosen = &repos.repos[i];
            break;
        }
    }
    if (chosen == nullptr) {
        chosen = &repos.repos[0];  // already sorted most-recently-pushed first
    }

    if (!chosen->run_known || chosen->run_state == RunState::None) {
        // No Actions to report. The push time is still the honest answer to "last worked on".
        char age[32];
        formatRelativeAge(age, sizeof(age), chosen->pushed_utc,
                          timeutil::systemTimeValid() ? timeutil::nowUtc() : 0);
        out.primary.assign(chosen->name.c_str());
        char line[64];
        std::snprintf(line, sizeof(line), "Pushed %s", age[0] != '\0' ? age : "at some point");
        out.secondary.assign(line);
        out.tone = dashboard::SummaryTone::Neutral;
        return;
    }

    out.primary.assign(runStateText(chosen->run_state));
    char line[64];
    std::snprintf(line, sizeof(line), "%s", chosen->name.c_str());
    out.secondary.assign(line);

    switch (chosen->run_state) {
        case RunState::Success:
            out.tone = dashboard::SummaryTone::Good;
            break;
        case RunState::Failure:
            out.tone = dashboard::SummaryTone::Bad;
            break;
        case RunState::Queued:
        case RunState::InProgress:
            out.tone = dashboard::SummaryTone::Warn;
            break;
        default:
            out.tone = dashboard::SummaryTone::Neutral;
            break;
    }
}

}  // namespace plugins
