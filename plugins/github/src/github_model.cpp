#include "plugins/github_model.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dashboard/json_util.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;
namespace timeutil = dashboard::timeutil;

/// Case-insensitive equality for GitHub logins, which are case-insensitive.
bool sameLogin(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

/// Bounded copy. Used instead of snprintf("%s") throughout this file for the same reason as in
/// elizabeth_model.cpp: -Werror=format-truncation reasons about worst-case widths and rejects the
/// obvious formulation.
void copyBounded(const char* src, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (src == nullptr) {
        out[0] = '\0';
        return;
    }
    const size_t len = std::strlen(src);
    const size_t copy = (len < capacity - 1) ? len : capacity - 1;
    std::memcpy(out, src, copy);
    out[copy] = '\0';
}

/// Insertion-sort a repo into the list, most recently pushed first, keeping at most kMaxRepos.
///
/// Same shape as insertDeparture()/insertHint() in elizabeth_model.cpp and for the same reason: a
/// cap that keeps the FIRST N rather than the BEST N silently drops the entries that matter when
/// the response is bigger than the board.
void insertRepo(RepoList& list, const RepoEntry& entry) {
    size_t position = list.count;
    while (position > 0 && list.repos[position - 1].pushed_utc < entry.pushed_utc) {
        --position;
    }
    if (position >= RepoList::kMaxRepos) {
        return;  // older than everything already held, and the list is full
    }

    const size_t last = (list.count < RepoList::kMaxRepos) ? list.count : RepoList::kMaxRepos - 1;
    for (size_t i = last; i > position; --i) {
        list.repos[i] = list.repos[i - 1];
    }
    list.repos[position] = entry;
    if (list.count < RepoList::kMaxRepos) {
        ++list.count;
    }
}

}  // namespace

const char* runStateText(RunState state) {
    switch (state) {
        case RunState::None:
            return "No actions";
        case RunState::Queued:
            return "Queued";
        case RunState::InProgress:
            return "Running";
        case RunState::Success:
            return "Passed";
        case RunState::Failure:
            return "Failed";
        case RunState::Cancelled:
            return "Cancelled";
        case RunState::Skipped:
            return "Skipped";
        case RunState::Unknown:
        default:
            return "Unknown";
    }
}

bool runStateBusy(RunState state) {
    return state == RunState::Queued || state == RunState::InProgress;
}

RunState runStateFrom(const char* status, const char* conclusion) {
    // STATUS FIRST. Anything not yet completed has a null conclusion, so testing conclusion first
    // would classify every running job as Unknown — see note 2 in the header.
    if (status != nullptr) {
        if (std::strcmp(status, "queued") == 0 || std::strcmp(status, "pending") == 0 ||
            std::strcmp(status, "requested") == 0 || std::strcmp(status, "waiting") == 0) {
            return RunState::Queued;
        }
        if (std::strcmp(status, "in_progress") == 0) {
            return RunState::InProgress;
        }
    }

    if (conclusion == nullptr || conclusion[0] == '\0') {
        return RunState::Unknown;
    }
    if (std::strcmp(conclusion, "success") == 0) {
        return RunState::Success;
    }
    if (std::strcmp(conclusion, "failure") == 0 || std::strcmp(conclusion, "timed_out") == 0 ||
        std::strcmp(conclusion, "startup_failure") == 0) {
        return RunState::Failure;
    }
    if (std::strcmp(conclusion, "cancelled") == 0) {
        return RunState::Cancelled;
    }
    if (std::strcmp(conclusion, "skipped") == 0 || std::strcmp(conclusion, "neutral") == 0) {
        return RunState::Skipped;
    }
    return RunState::Unknown;
}

std::time_t parseIso8601Utc(const char* text) {
    // Exactly "YYYY-MM-DDTHH:MM:SSZ". Checked digit by digit rather than sscanf'd so a subtly
    // different shape is a clean failure rather than a plausible wrong answer.
    if (text == nullptr || std::strlen(text) < 20) {
        return 0;
    }
    for (int i = 0; i < 19; ++i) {
        const bool want_digit = (i != 4 && i != 7 && i != 10 && i != 13 && i != 16);
        const char c = text[i];
        if (want_digit) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                return 0;
            }
        } else if (i == 10) {
            if (c != 'T' && c != ' ') {
                return 0;
            }
        } else if ((i == 4 || i == 7) && c != '-') {
            return 0;
        } else if ((i == 13 || i == 16) && c != ':') {
            return 0;
        }
    }

    // Each field read from its own offset: atoi on the whole string would stop at the first '-'
    // and give only the year.
    char field[5];
    copyBounded(text, field, 5);
    const int y = std::atoi(field);
    copyBounded(text + 5, field, 3);
    const int mon = std::atoi(field);
    copyBounded(text + 8, field, 3);
    const int day = std::atoi(field);
    copyBounded(text + 11, field, 3);
    const int hour = std::atoi(field);
    copyBounded(text + 14, field, 3);
    const int minute = std::atoi(field);
    copyBounded(text + 17, field, 3);
    const int second = std::atoi(field);

    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        return 0;
    }

    // timegm() is not available here, and mktime() would apply the local timezone to a value that
    // is explicitly UTC. daysFromCivil() is the same routine the rest of the project uses.
    const long long days = timeutil::daysFromCivil(y, static_cast<unsigned>(mon),
                                                   static_cast<unsigned>(day));
    return static_cast<std::time_t>(days * 86400LL + hour * 3600LL + minute * 60LL + second);
}

bool parseRepos(const char* json_text, size_t len, const char* username, RepoList& out) {
    out = RepoList{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }
    const cJSON* root = doc.root();
    if (!cJSON_IsArray(root)) {
        return false;
    }

    const size_t total = json::arraySize(root);
    for (size_t i = 0; i < total; ++i) {
        const cJSON* node = json::at(root, i);
        if (node == nullptr) {
            continue;
        }

        RepoEntry entry;
        if (!json::string(node, "name", entry.name)) {
            continue;  // a repository with no name is not something to guess about
        }
        json::string(node, "full_name", entry.full_name);
        json::boolean(node, "private", entry.private_repo);

        char stamp[40];
        if (json::string(node, "pushed_at", stamp, sizeof(stamp))) {
            entry.pushed_utc = parseIso8601Utc(stamp);
        }

        const cJSON* owner = json::object(node, "owner");
        if (owner != nullptr) {
            char login[64];
            if (json::string(owner, "login", login, sizeof(login))) {
                entry.owned_by_user = sameLogin(login, username);
            }
        }

        insertRepo(out, entry);
    }

    out.valid = true;
    return true;
}

bool parseRuns(const char* json_text, size_t len, RunList& out) {
    const dashboard::MediumString keep_name = out.full_name;
    out = RunList{};
    out.full_name = keep_name;

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    const cJSON* runs = json::array(doc.root(), "workflow_runs");
    if (runs == nullptr) {
        return false;
    }

    const size_t total = json::arraySize(runs);
    for (size_t i = 0; i < total && out.count < RunList::kMaxRuns; ++i) {
        const cJSON* node = json::at(runs, i);
        if (node == nullptr) {
            continue;
        }

        RunEntry entry;
        char status[32] = {};
        char conclusion[32] = {};
        json::string(node, "status", status, sizeof(status));
        json::string(node, "conclusion", conclusion, sizeof(conclusion));
        entry.state = runStateFrom(status, conclusion);

        json::string(node, "name", entry.workflow);

        // display_title is the commit subject for a push run and the run name otherwise, which
        // is the more useful of the two for a board. Fall back to the head commit message.
        if (!json::string(node, "display_title", entry.title)) {
            const cJSON* head = json::object(node, "head_commit");
            if (head != nullptr) {
                json::string(head, "message", entry.title);
            }
        }

        char stamp[40];
        if (json::string(node, "updated_at", stamp, sizeof(stamp))) {
            entry.updated_utc = parseIso8601Utc(stamp);
        }
        if (entry.updated_utc == 0 && json::string(node, "created_at", stamp, sizeof(stamp))) {
            entry.updated_utc = parseIso8601Utc(stamp);
        }

        int64_t number = 0;
        if (json::integer64(node, "run_number", number)) {
            entry.number = number;
        }

        out.runs[out.count++] = entry;
    }

    // Valid even with zero runs: "this repository has no workflows" is an answer, not a failure.
    out.valid = true;
    return true;
}

void formatRelativeAge(char* out, size_t capacity, std::time_t then_utc, std::time_t now_utc) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (then_utc <= 0 || now_utc <= 0) {
        out[0] = '\0';
        return;
    }

    long long seconds = static_cast<long long>(now_utc) - static_cast<long long>(then_utc);
    if (seconds < 0) {
        // A run stamped in the future means the device clock is behind, not that the run has not
        // happened. "just now" is the least wrong thing to say.
        seconds = 0;
    }

    if (seconds < 60) {
        std::snprintf(out, capacity, "just now");
    } else if (seconds < 3600) {
        std::snprintf(out, capacity, "%lld min ago", seconds / 60);
    } else if (seconds < 86400) {
        const long long hours = seconds / 3600;
        std::snprintf(out, capacity, "%lld %s ago", hours, hours == 1 ? "hour" : "hours");
    } else {
        const long long days = seconds / 86400;
        std::snprintf(out, capacity, "%lld %s ago", days, days == 1 ? "day" : "days");
    }
}

}  // namespace plugins
