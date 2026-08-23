#include "plugins/github_provider.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

#include "dashboard/storage/secret_store.hpp"

namespace plugins {
namespace {

using dashboard::storage::Secret;
using dashboard::storage::SecretStore;

constexpr const char* kTag = "github";
constexpr const char* kHost = "https://api.github.com";

/// GitHub asks for an explicit API version and will otherwise pick one for you. Pinning it means
/// a future default cannot quietly change the response shape under a device nobody is watching.
constexpr const char* kApiVersionHeader = "X-GitHub-Api-Version";
constexpr const char* kApiVersion = "2022-11-28";

/// Enough for the longest URL built here: host + /repos/{owner}/{repo}/actions/runs + parameters.
constexpr size_t kUrlBytes = 320;

}  // namespace

bool GithubProvider::authenticated() { return SecretStore::has(Secret::GithubToken); }

esp_err_t GithubProvider::get(const char* url, const char* safe_label, char* buffer,
                              size_t capacity, size_t& length) {
    length = 0;

    dashboard::net::HttpRequest request;
    request.url = url;
    request.header_name = kApiVersionHeader;
    request.header_value = kApiVersion;

    // The token goes in `bearer`, which HttpsClient renders as "Authorization: Bearer ...", never
    // logs, and — since this build — refuses to carry across a redirect.
    char token[dashboard::storage::kMaxSecretLength + 1] = {};
    const bool have_token = SecretStore::get(Secret::GithubToken, token, sizeof(token)) == ESP_OK &&
                            token[0] != '\0';
    if (have_token) {
        request.bearer = token;
    }

    // One attempt. A refresh is eleven of these back to back; retrying each three times turns a
    // slow refresh into a minute and burns the rate limit that made it slow.
    request.max_attempts = 1;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);
    std::memset(token, 0, sizeof(token));

    if (err != ESP_OK) {
        // Distinguish the failures that mean something the owner can act on. Rate limiting is
        // reported as 403 with a remaining count of zero, or 429.
        switch (response.status) {
            case 401:
                last_error_ = "GitHub rejected the token";
                break;
            case 403:
            case 429:
                last_error_ = have_token ? "GitHub rate limit reached"
                                         : "rate limited - add a token in settings";
                break;
            case 404:
                last_error_ = "not found - check the username";
                break;
            default:
                last_error_ = response.truncated ? "response too large" : "GitHub unreachable";
                break;
        }
        ESP_LOGW(kTag, "%s: HTTP %d (%s)", safe_label, response.status, last_error_);
        return err;
    }

    length = response.length;
    last_error_ = "";
    return ESP_OK;
}

esp_err_t GithubProvider::fetchRepos(const char* username, bool all_repositories, char* buffer,
                                     size_t capacity, RepoList& out) {
    if (username == nullptr || username[0] == '\0') {
        last_error_ = "no GitHub username set";
        return ESP_ERR_INVALID_ARG;
    }

    char url[kUrlBytes];
    const bool token = authenticated();
    if (token && all_repositories) {
        // Everything the token can see. affiliation is spelled out rather than left to the
        // default so the behaviour does not change if GitHub changes that default.
        std::snprintf(url, sizeof(url),
                      "%s/user/repos?sort=pushed&direction=desc&per_page=%u"
                      "&affiliation=owner,collaborator,organization_member",
                      kHost, static_cast<unsigned>(RepoList::kMaxRepos));
    } else if (token) {
        std::snprintf(url, sizeof(url),
                      "%s/user/repos?sort=pushed&direction=desc&per_page=%u&affiliation=owner",
                      kHost, static_cast<unsigned>(RepoList::kMaxRepos));
    } else {
        // No token: only public repositories owned by this user are reachable at all, so the
        // filter cannot be honoured and the "all" case degrades to the same request.
        std::snprintf(url, sizeof(url), "%s/users/%s/repos?sort=pushed&direction=desc&per_page=%u",
                      kHost, username, static_cast<unsigned>(RepoList::kMaxRepos));
    }

    size_t length = 0;
    const esp_err_t err = get(url, "repos", buffer, capacity, length);
    if (err != ESP_OK) {
        return err;
    }

    if (!parseRepos(buffer, length, username, out)) {
        last_error_ = "repository list not understood";
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(kTag, "%u repositories from %u bytes (%s)", static_cast<unsigned>(out.count),
             static_cast<unsigned>(length), token ? "authenticated" : "public only");
    return ESP_OK;
}

esp_err_t GithubProvider::fetchLatestRun(const char* full_name, char* buffer, size_t capacity,
                                         RepoEntry& entry) {
    if (full_name == nullptr || full_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char url[kUrlBytes];
    std::snprintf(url, sizeof(url),
                  "%s/repos/%s/actions/runs?per_page=1&exclude_pull_requests=true", kHost,
                  full_name);

    size_t length = 0;
    const esp_err_t err = get(url, "latest run", buffer, capacity, length);
    if (err != ESP_OK) {
        return err;
    }

    RunList runs;
    if (!parseRuns(buffer, length, runs)) {
        last_error_ = "run list not understood";
        return ESP_ERR_INVALID_RESPONSE;
    }

    // run_known regardless of whether there were any runs: it records that we ASKED, which is
    // what lets the row stop saying "checking" and start saying "no actions".
    entry.run_known = true;
    if (runs.count == 0) {
        entry.run_state = RunState::None;
        entry.run_workflow.clear();
        entry.run_updated_utc = 0;
        return ESP_OK;
    }

    entry.run_state = runs.runs[0].state;
    entry.run_workflow = runs.runs[0].workflow;
    entry.run_updated_utc = runs.runs[0].updated_utc;
    return ESP_OK;
}

esp_err_t GithubProvider::fetchRuns(const char* full_name, char* buffer, size_t capacity,
                                    RunList& out) {
    if (full_name == nullptr || full_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char url[kUrlBytes];
    std::snprintf(url, sizeof(url),
                  "%s/repos/%s/actions/runs?per_page=%u&exclude_pull_requests=true", kHost,
                  full_name, static_cast<unsigned>(RunList::kMaxRuns));

    size_t length = 0;
    const esp_err_t err = get(url, "runs", buffer, capacity, length);
    if (err != ESP_OK) {
        return err;
    }

    out.full_name.assign(full_name);
    if (!parseRuns(buffer, length, out)) {
        last_error_ = "run list not understood";
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(kTag, "%u runs for %s from %u bytes", static_cast<unsigned>(out.count), full_name,
             static_cast<unsigned>(length));
    return ESP_OK;
}

}  // namespace plugins
