// GitHub repositories and their Actions runs, and the parsers that produce them.
//
// ESP-free and LVGL-free, like weather_model.hpp and elizabeth_model.hpp, so test/host can
// compile it unchanged and the parsers can be exercised against saved fixtures.
//
// WHAT THE GITHUB API ACTUALLY RETURNS — measured, not assumed. These decided the design:
//
//  1. **Run objects are enormous.** Each one embeds the complete repository object AND the head
//     commit, so a single run is ~20 KB and five are ~99 KB (73 KB with exclude_pull_requests).
//     That is why dash::cfg::kHttpMaxResponseBytes had to be raised, and why the repo list and
//     the run list are fetched separately rather than in one pass.
//
//  2. **Status and conclusion are two different fields, and both are needed.** `status` is
//     queued / in_progress / completed and says whether it is RUNNING; `conclusion` is
//     success / failure / cancelled / skipped / null and says whether it WORKED. A run that is
//     in progress has a null conclusion — reading only `conclusion` would report every running
//     job as "no result", which is exactly the state the page is meant to make visible.
//
//  3. **A repository can have no workflows at all**, in which case `actions/runs` returns
//     `{"total_count": 0, "workflow_runs": []}`. That is a normal, common answer and not an
//     error — most repositories do not use Actions. The page says "no actions" for those rather
//     than showing them as broken.
//
//  4. **`pushed_at` is the useful "last worked on" signal**, not `updated_at`. `updated_at`
//     moves when metadata changes — a description edit, a star, a rename — so sorting by it
//     surfaces repositories nobody has touched. `sort=pushed` on the request and `pushed_at` in
//     the model both mean commits.
//
//  5. **Timestamps are ISO 8601 with a Z suffix** ("2026-08-23T14:31:02Z"), not epoch seconds.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "dashboard/fixed_string.hpp"

namespace plugins {

/// What an Actions run is doing, collapsed from GitHub's status + conclusion pair.
///
/// Ordered so that "is something happening right now" is a single comparison — see running().
enum class RunState : uint8_t {
    None,        ///< This repository has no workflow runs at all. Not a failure.
    Queued,      ///< Accepted, not started.
    InProgress,  ///< Running now. This is the "currently being updated" state the page shows.
    Success,
    Failure,
    Cancelled,
    Skipped,
    Unknown,  ///< A status or conclusion this firmware does not know. Shown verbatim-ish.
};

/// Short label for the board: "Running", "Passed", "Failed".
const char* runStateText(RunState state);

/// True while GitHub is still working on it — queued or in progress.
bool runStateBusy(RunState state);

/// Map GitHub's `status` and `conclusion` onto one state.
///
/// `conclusion` may be null or absent, which is normal and expected for anything not completed.
RunState runStateFrom(const char* status, const char* conclusion);

/// Parse an ISO 8601 UTC timestamp ("2026-08-23T14:31:02Z") to epoch seconds. 0 on failure.
///
/// Deliberately strict about the shape rather than calling strptime: the format is fixed by the
/// API, and a lenient parser that silently returns a wrong time would make "3 minutes ago" read
/// as "51 years ago" with no clue why.
std::time_t parseIso8601Utc(const char* text);

// ---------------------------------------------------------------------------------------
// Repositories
// ---------------------------------------------------------------------------------------

struct RepoEntry {
    /// Just the name, for the row: "m5Dash".
    dashboard::ShortString name;

    /// "owner/name" — needed for every follow-up request and to tell two same-named repos apart.
    dashboard::MediumString full_name;

    /// Last commit push, UTC. The "worked on" time — see note 4.
    std::time_t pushed_utc = 0;

    /// Owned by the configured username, as opposed to reached through an organisation or as a
    /// collaborator. Drives the My / All filter without a second request.
    bool owned_by_user = false;

    bool private_repo = false;

    // ---- latest Actions run, filled by a second request per repo ------------------------
    /// None until asked for. The row renders progressively: the list appears first and each
    /// status fills in as its request lands, because eleven serialised TLS requests take too
    /// long to leave the page blank for.
    RunState run_state = RunState::None;
    bool run_known = false;  ///< A runs request has completed for this repo, whatever it said.
    dashboard::ShortString run_workflow;
    std::time_t run_updated_utc = 0;

    /// Who set the latest run going — the GitHub login, so it matches what you see on the site.
    ///
    /// Free: it is already in the `actions/runs` response the status comes from, so showing it
    /// costs no extra request. Empty for a repository with no runs, where there is nothing to
    /// attribute; finding the last pusher THERE would need a separate /commits call per
    /// repository, which is not worth another six requests a refresh.
    dashboard::ShortString run_actor;
};

struct RepoList {
    /// Six, as asked for -- per list, and there are two lists (mine and work).
    ///
    /// Also a rate-limit decision: one refresh costs 1 + count requests, so six is seven
    /// requests and about twelve seconds of serialised TLS. Unauthenticated GitHub allows 60 an
    /// hour, which ten would have eaten through in five refreshes.
    static constexpr size_t kMaxRepos = 6;

    RepoEntry repos[kMaxRepos];
    size_t count = 0;
    bool valid = false;
};

/// Parse `GET /user/repos` or `GET /users/{u}/repos`.
///
/// `username` decides `owned_by_user` by comparing owner.login case-insensitively — GitHub logins
/// are case-insensitive and the API does not promise a particular case back.
///
/// Entries arrive already sorted by the `sort=pushed` request parameter, but this re-sorts by
/// pushed_utc anyway: relying on server order means a silently wrong board the day the parameter
/// is dropped or ignored.
bool parseRepos(const char* json, size_t len, const char* username, RepoList& out);

// ---------------------------------------------------------------------------------------
// Runs
// ---------------------------------------------------------------------------------------

struct RunEntry {
    RunState state = RunState::Unknown;
    dashboard::ShortString workflow;

    /// Who triggered this run.
    ///
    /// `actor.login` where present, falling back to the head commit's author name. Note that for
    /// a SCHEDULED run this is whichever account owns the schedule — often a bot — rather than a
    /// person who pushed anything. That is genuinely who triggered it, so it is shown as-is
    /// rather than blanked: "espressif-bot ran this on a timer" is information.
    dashboard::ShortString actor;

    /// The commit subject or run name — what the run was FOR. Truncated for display.
    dashboard::MediumString title;

    std::time_t updated_utc = 0;

    /// GitHub's own per-repository run number, shown as "#42" so a row can be matched against
    /// the website.
    int64_t number = 0;
};

struct RunList {
    /// Five, as asked for.
    static constexpr size_t kMaxRuns = 5;

    RunEntry runs[kMaxRuns];
    size_t count = 0;
    bool valid = false;

    /// The repository these belong to, so a late response can be discarded if the user has
    /// already drilled into a different repo.
    dashboard::MediumString full_name;
};

/// Parse `GET /repos/{owner}/{repo}/actions/runs`.
///
/// Fills at most kMaxRuns, newest first, which is the order GitHub returns. `valid` is set even
/// when count is 0 — see note 3: a repository with no workflows is a real answer.
bool parseRuns(const char* json, size_t len, RunList& out);

/// Turn a GitHub login into the name you would actually call the person.
///
/// `aliases` is a comma-separated list of `login=Name` pairs, e.g.
///
///     colgateteeth200=Yusuf,morfry=Morgan,amore55=Moreno
///
/// Writes the matching name into `out`, or the login unchanged when there is no entry for it —
/// never blank, because an unmapped colleague should still be identifiable.
///
/// Matching is case-insensitive, GitHub logins being case-insensitive, and surrounding spaces are
/// ignored so the list can be written readably. A malformed entry is skipped rather than failing
/// the whole list: one typo should not un-name everybody.
void displayNameFor(const char* aliases, const char* login, char* out, size_t capacity);

/// "3 min ago", "4 h ago", "2 days ago", "just now". Empty for an unknown time.
///
/// Lives here rather than in the plugin because it is pure formatting over a duration and is the
/// kind of thing worth a host test.
void formatRelativeAge(char* out, size_t capacity, std::time_t then_utc, std::time_t now_utc);

}  // namespace plugins
