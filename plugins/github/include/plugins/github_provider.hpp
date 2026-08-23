// The GitHub REST API, behind an interface, so the page holds no HTTP and no JSON.
//
// A TOKEN IS OPTIONAL AND THE PAGE IS THIN WITHOUT ONE. Unauthenticated the API returns only
// PUBLIC repositories owned by the named user and allows 60 requests an hour; one refresh of ten
// repositories costs eleven, so that is five refreshes an hour before the limit bites. With a
// token: private and organisation repositories, the All-repositories filter, and 5000/hr.
//
// WHY ELEVEN REQUESTS. There is no REST endpoint that returns "latest Actions run for each of my
// repositories" — runs are per repository. So the page fetches the repository list once and then
// one `actions/runs?per_page=1` per repository. The alternative is GraphQL, which would do it in
// one query, but GraphQL REQUIRES a token even for public data, and demands a hand-built query
// document and a different response shape. Not worth it while REST works unauthenticated.
//
// Requests are issued one at a time and each is a fresh TLS handshake, serialised device-wide by
// the gate in https_client.cpp. A full refresh therefore takes on the order of fifteen seconds,
// which is why the plugin renders the repository list as soon as it arrives and lets the run
// states fill in behind it.

#pragma once

#include <cstddef>

#include "esp_err.h"

#include "dashboard/net/https_client.hpp"
#include "plugins/github_model.hpp"

namespace plugins {

/// Which of the two lists — and therefore which of the two tokens — a request is for.
///
/// Kept separate because a fine-grained token has exactly ONE resource owner: the personal token
/// physically cannot see organisation repositories and the work token cannot see personal ones,
/// so there is no single request that answers both.
enum class RepoScope : uint8_t {
    Mine,
    Work,
};

class GithubProvider {
  public:
    /// Ceiling for a repository list. Measured: ten repositories is 61,650 bytes, so this is a
    /// little over 1.5x the worst observation. See the table in app_config.hpp.
    static constexpr size_t kReposResponseBytes = 96 * 1024;

    /// Ceiling for a runs response. Measured: one run is 19,827 bytes and five are 72,888 with
    /// exclude_pull_requests, because every run embeds its whole repository and head commit.
    static constexpr size_t kRunsResponseBytes = 112 * 1024;

    /// True when a token is stored for that scope.
    static bool authenticated(RepoScope scope);

    /// The repository list for one scope. Worker thread.
    ///
    /// Mine, with a token:      /user/repos?affiliation=owner
    /// Mine, without:           /users/{username}/repos      (public repositories only)
    /// Work, with `organisation`:  /orgs/{organisation}/repos
    /// Work, without:           /user/repos?affiliation=organization_member
    ///
    /// The organisation form is preferred for work because it is the reliable route for a
    /// fine-grained token owned by that organisation; the affiliation form needs the token to be
    /// able to enumerate the user's organisations and can legitimately return nothing.
    esp_err_t fetchRepos(RepoScope scope, const char* username, const char* organisation,
                         char* buffer, size_t capacity, RepoList& out);

    /// The latest run for one repository, written into `entry`. Worker thread.
    ///
    /// `scope` selects the token: a repository found through the work token must be queried with
    /// it, because the personal token cannot see it at all.
    ///
    /// A repository with no workflows sets RunState::None and returns ESP_OK — that is an answer,
    /// not a failure.
    esp_err_t fetchLatestRun(RepoScope scope, const char* full_name, char* buffer, size_t capacity,
                             RepoEntry& entry);

    /// The last few runs for one repository, for the drill-down. Worker thread.
    esp_err_t fetchRuns(RepoScope scope, const char* full_name, char* buffer, size_t capacity,
                        RunList& out);

    /// Short, user-facing reason for the last failure. Never contains a URL or a token.
    const char* lastError() const { return last_error_; }

  private:
    /// Perform a GET against api.github.com with the standard headers, and that scope's stored
    /// token when there is one. Copies the token onto the stack for the call and wipes it after.
    esp_err_t get(RepoScope scope, const char* url, const char* safe_label, char* buffer,
                  size_t capacity, size_t& length);

    dashboard::net::HttpsClient http_;
    const char* last_error_ = "";
};

}  // namespace plugins
