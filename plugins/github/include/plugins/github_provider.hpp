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

class GithubProvider {
  public:
    /// Ceiling for a repository list. Measured: ten repositories is 61,650 bytes, so this is a
    /// little over 1.5x the worst observation. See the table in app_config.hpp.
    static constexpr size_t kReposResponseBytes = 96 * 1024;

    /// Ceiling for a runs response. Measured: one run is 19,827 bytes and five are 72,888 with
    /// exclude_pull_requests, because every run embeds its whole repository and head commit.
    static constexpr size_t kRunsResponseBytes = 112 * 1024;

    /// True when a personal access token is stored.
    static bool authenticated();

    /// The repository list. Worker thread.
    ///
    /// `all_repositories` only means anything with a token: it selects /user/repos, which covers
    /// everything the token can see. Without a token it is ignored and /users/{username}/repos is
    /// used, because there is no way to ask for someone else's private or org repositories.
    esp_err_t fetchRepos(const char* username, bool all_repositories, char* buffer, size_t capacity,
                         RepoList& out);

    /// The latest run for one repository, written into `entry`. Worker thread.
    ///
    /// A repository with no workflows sets RunState::None and returns ESP_OK — that is an answer,
    /// not a failure.
    esp_err_t fetchLatestRun(const char* full_name, char* buffer, size_t capacity,
                             RepoEntry& entry);

    /// The last few runs for one repository, for the drill-down. Worker thread.
    esp_err_t fetchRuns(const char* full_name, char* buffer, size_t capacity, RunList& out);

    /// Short, user-facing reason for the last failure. Never contains a URL or a token.
    const char* lastError() const { return last_error_; }

  private:
    /// Perform a GET against api.github.com with the standard headers, and the stored token when
    /// there is one. Copies the token onto the stack for the duration of the call and wipes it.
    esp_err_t get(const char* url, const char* safe_label, char* buffer, size_t capacity,
                  size_t& length);

    dashboard::net::HttpsClient http_;
    const char* last_error_ = "";
};

}  // namespace plugins
