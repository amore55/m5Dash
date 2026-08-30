#include "plugins/telegram_service.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>  // strncasecmp — POSIX, not in <cstring>/std::

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.hpp"
#include "dashboard/json_util.hpp"
#include "dashboard/net/response_buffer.hpp"
#include "dashboard/storage/cache_store.hpp"
#include "dashboard/storage/secret_store.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;
namespace timeutil = dashboard::timeutil;
using dashboard::storage::CacheStore;
using dashboard::storage::Secret;
using dashboard::storage::SecretStore;
using dashboard::storage::Task;
using dashboard::storage::TaskSource;
using dashboard::storage::TaskStore;

constexpr const char* kTag = "telegram";
constexpr const char* kHost = "https://api.telegram.org";

/// Persists next_update_id_ across a reboot. Reused from CacheStore rather than a purpose-built
/// store for eight bytes — see task_store.hpp's own note on not over-building storage for a
/// small need. A pause between getUpdates calls when nothing is configured or the network is
/// down, so an unconfigured bot does not spin-loop hitting Telegram (or a dead radio) for no
/// reason.
constexpr const char* kOffsetCacheKey = "tg_offset";
constexpr uint32_t kIdlePauseMs = 5000;
constexpr uint32_t kErrorBackoffMs = 5000;

/// getUpdates responses are small, fixed-shape JSON, but several messages can queue up while the
/// bot was unreachable — sized with real headroom for a personal, low-volume bot rather than
/// measured against a live account, matching the caution the calendar and OTA manifests document
/// about guessing versus measuring.
constexpr size_t kResponseBytes = 8 * 1024;

/// How many open tasks /list fetches before saying "and N more". Deliberately small: each is a
/// full dashboard::storage::Task (a TextString title alone is 192 bytes) sitting on this worker's
/// stack, and the REPLY TEXT buffer (below) could not display many more than this anyway before
/// running short itself — raising this without also raising the reply buffer just spends stack
/// fetching rows that would never be shown.
constexpr size_t kListReplyMax = 12;

/// Percent-encode for an application/x-www-form-urlencoded body.
///
/// Needed here and nowhere else so far: every other POST body in this project (OAuth's device
/// code, refresh token, client ID) is a value ISSUED BY the server we are sending it back to and
/// therefore already URL-safe by construction — see graph_calendar_provider.cpp's note on why it
/// has no encoder. A Telegram reply is built from a task TITLE the user typed, which can contain
/// spaces, '&', unicode — anything. This is the first real need for one, not a general-purpose
/// utility built ahead of a use.
void percentEncode(const char* in, char* out, size_t capacity) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 4 < capacity; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const bool safe = (std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out[o++] = static_cast<char>(c);
        } else {
            std::snprintf(out + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = '\0';
}

/// Trim ASCII whitespace from both ends, in place.
void trim(char* text) {
    char* start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }
    size_t len = std::strlen(start);
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                       start[len - 1] == '\r' || start[len - 1] == '\n')) {
        --len;
    }
    std::memmove(text, start, len);
    text[len] = '\0';
}

/// Does `text` start with `command`, case-insensitively, followed by either end-of-string or a
/// space? Rules out "/donee" matching "/done".
bool startsWithCommand(const char* text, const char* command) {
    const size_t len = std::strlen(command);
    if (strncasecmp(text, command, len) != 0) {
        return false;
    }
    return text[len] == '\0' || text[len] == ' ';
}

}  // namespace

esp_err_t TelegramService::start(dashboard::storage::TaskStore& tasks) {
    int64_t stored = 0;
    size_t len = 0;
    if (CacheStore::get(kOffsetCacheKey, &stored, sizeof(stored), &len) == ESP_OK &&
        len == sizeof(stored)) {
        next_update_id_ = stored;
    }
    // 16 KB. 8 KB — this project's usual convention for "a TLS handshake plus a parse" — was
    // tried first and OVERFLOWED on a real device processing a real message: a genuine FreeRTOS
    // stack-protection panic in the "telegram" task, not the separate internal-SRAM class of
    // crash this feature also hit earlier. Every test up to that point had failed before ever
    // reaching handleMessage() (no token, then a malformed one, then a network-ordering bug), so
    // 8 KB had never actually been exercised against the real path — mbedTLS's own call depth
    // during the handshake, on top of pollOnce()'s token/url/text/reply locals (~2.7 KB) and
    // attemptGet()'s own auth[512] in https_client.cpp, added up to more than weather's plain
    // 8 KB budget covers for a smaller set of locals. See the high-water-mark log below rather
    // than re-deriving this by hand again.
    const esp_err_t err = worker_.start("telegram", 16384);
    if (err != ESP_OK) {
        return err;
    }

    // ONE job, which never returns — pollLoop() runs for the lifetime of the device, exactly as
    // the header explains. Everything this service does happens inside that one job; nothing
    // else is ever posted to this worker.
    dashboard::storage::TaskStore* tasks_ptr = &tasks;
    worker_.post([this, tasks_ptr] { pollLoop(tasks_ptr); });
    return ESP_OK;
}

void TelegramService::pollLoop(dashboard::storage::TaskStore* tasks) {
    for (;;) {
        dashboard::net::ResponseBuffer buffer(kResponseBytes);
        esp_err_t err = ESP_ERR_NO_MEM;
        if (buffer.valid()) {
            err = pollOnce(*tasks, buffer.data(), buffer.capacity());
        }

        if (err == ESP_ERR_INVALID_STATE) {
            // Not configured (no token yet, or none stored). Nothing to poll — wait rather than
            // hammering this check every iteration.
            vTaskDelay(pdMS_TO_TICKS(kIdlePauseMs));
        } else if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(kErrorBackoffMs));
        }
        // On ESP_OK, no delay at all: the long poll itself already waited up to
        // dash::cfg::kTelegramLongPollSeconds inside http_.get(), so looping straight back into
        // the next call is correct long-poll behaviour, not a busy-loop.
    }
}

esp_err_t TelegramService::pollOnce(dashboard::storage::TaskStore& tasks, char* buffer,
                                    size_t capacity) {
    char token[dashboard::storage::kMaxSecretLength + 1] = {};
    if (SecretStore::get(Secret::TelegramBotToken, token, sizeof(token)) != ESP_OK ||
        token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[420];  // host + /bot<token up to 256> + getUpdates path/query, generous for -Werror=format-truncation
    std::snprintf(url, sizeof(url),
                 "%s/bot%s/getUpdates?offset=%lld&timeout=%u"
                 "&allowed_updates=%%5B%%22message%%22%%5D",
                 kHost, token, static_cast<long long>(next_update_id_),
                 static_cast<unsigned>(dash::cfg::kTelegramLongPollSeconds));

    dashboard::net::HttpRequest request;
    request.url = url;
    request.path_is_sensitive = true;  // the token is IN this URL — see the file header
    request.timeout_ms =
        static_cast<int>((dash::cfg::kTelegramLongPollSeconds + 10) * 1000);
    request.max_attempts = 1;  // a long-poll timing out is routine, not a failure worth retrying

    dashboard::net::HttpResponse response;
    const esp_err_t transport_err = http_.get(request, buffer, capacity, response);
    // The URL is wiped too — the token was embedded in it, not just in `token`.
    std::memset(url, 0, sizeof(url));

    if (transport_err != ESP_OK) {
        std::memset(token, 0, sizeof(token));
        return transport_err;
    }

    dashboard::json::Doc doc;
    if (!doc.parse(buffer, response.length)) {
        std::memset(token, 0, sizeof(token));
        ESP_LOGW(kTag, "getUpdates response did not parse");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON* results = json::array(doc.root(), "result");
    const size_t count = json::arraySize(results);
    bool changed = false;

    for (size_t i = 0; i < count; ++i) {
        const cJSON* item = json::at(results, i);
        int64_t update_id = 0;
        if (item == nullptr || !json::integer64(item, "update_id", update_id)) {
            continue;  // malformed entry; nothing to advance past, so leave the offset alone
        }
        // Advanced regardless of what follows — an update this bot does not understand (no
        // "message" field: an edited message, a channel post) must not be fetched forever.
        next_update_id_ = update_id + 1;

        const cJSON* message = json::object(item, "message");
        if (message == nullptr) {
            continue;
        }
        int64_t from_id = 0;
        int64_t chat_id = 0;
        json::integer64(json::object(message, "from"), "id", from_id);
        json::integer64(json::object(message, "chat"), "id", chat_id);

        char text[512] = {};
        if (chat_id == 0 || !json::string(message, "text", text, sizeof(text))) {
            continue;  // no text: a photo, a sticker, something this bot has nothing to say about
        }

        char reply[1536] = {};
        const bool mutated = handleMessage(tasks, from_id, text, update_id, reply, sizeof(reply));
        changed = changed || mutated;

        if (reply[0] != '\0') {
            sendReply(token, chat_id, reply, buffer, capacity);
        }

        // Logged once, not every message: this is the actual deepest path this task runs (a real
        // message plus its reply), and the 16 KB budget above was sized by reasoning about it
        // rather than measuring it — see that comment. Bytes REMAINING at the shallowest point
        // FreeRTOS happened to sample, so a low number here is real headroom eaten, not noise.
        static bool logged_high_water = false;
        if (!logged_high_water) {
            logged_high_water = true;
            ESP_LOGI(kTag, "stack headroom after a real message: %u B remaining",
                    static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
        }
    }

    std::memset(token, 0, sizeof(token));

    // Persisted even when nothing matched, so a batch of updates this bot skipped (no message
    // field, an unauthorised sender) is not re-fetched on every single cycle forever.
    CacheStore::put(kOffsetCacheKey, &next_update_id_, sizeof(next_update_id_));

    if (changed && on_changed_) {
        on_changed_();
    }
    return ESP_OK;
}

bool TelegramService::handleMessage(dashboard::storage::TaskStore& tasks, int64_t from_id,
                                    char* text, int64_t update_id, char* reply,
                                    size_t reply_capacity) {
    const int64_t allowed = allowed_user_id_.load();
    if (allowed == 0 || from_id != allowed) {
        // Silent, deliberately — see the file header. Not even an error reply, which would
        // confirm to a stranger that the bot exists and is listening.
        reply[0] = '\0';
        return false;
    }

    // Trimmed IN PLACE — `text` is a local array in the caller's frame with nothing else that
    // needs its untrimmed form, so a separate copy just to trim it would cost real stack for no
    // benefit (a /list reply already spends real stack of its own — see kListReplyMax below).
    char* body = text;
    trim(body);
    if (body[0] == '\0') {
        reply[0] = '\0';
        return false;
    }

    if (body[0] != '/') {
        // Plain text: a new task. "tg-<update_id>" makes replaying this same update idempotent
        // — see Task::id's own doc comment.
        Task task;
        // 32, not 24 (Task::id's own capacity): -Werror=format-truncation reasons about %lld's
        // worst case as a full signed 64-bit range (20 digits plus a sign), and update_id is
        // always non-negative and realistically well under half that length regardless.
        // FixedString::assign() below truncates safely either way.
        char id[32];
        std::snprintf(id, sizeof(id), "tg-%lld", static_cast<long long>(update_id));
        task.id.assign(id);
        task.title.assign(body);
        task.source = TaskSource::Telegram;
        task.created_at = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;

        const esp_err_t err = tasks.upsert(task);
        if (err == ESP_ERR_NO_MEM) {
            std::snprintf(reply, reply_capacity,
                         "Task list is full (%u). Clear some completed ones with /clear first.",
                         static_cast<unsigned>(dash::cfg::kMaxTasks));
            return false;
        }
        std::snprintf(reply, reply_capacity, "Added: %s", task.title.c_str());
        return err == ESP_OK;
    }

    if (startsWithCommand(body, "/list")) {
        Task open[kListReplyMax];
        const size_t shown = tasks.snapshot(open, kListReplyMax, /*open_only=*/true);
        const size_t total_open = tasks.openCount();
        if (shown == 0) {
            std::snprintf(reply, reply_capacity, "Nothing open. Well done.");
            return false;
        }
        size_t written = 0;
        for (size_t i = 0; i < shown && written + 64 < reply_capacity; ++i) {
            char shortid[8];
            open[i].shortId(shortid, sizeof(shortid));
            const int printed = std::snprintf(reply + written, reply_capacity - written,
                                              "%s  %s\n", shortid, open[i].title.c_str());
            // snprintf's contract allows a negative return on an encoding error. Treating that
            // as "wrote nothing" and stopping is what keeps `written` (size_t) from wrapping to
            // a huge value and turning `reply + written` into an out-of-bounds pointer next
            // iteration — the one thing this loop must never do with attacker-influenced text.
            if (printed < 0) {
                break;
            }
            written += static_cast<size_t>(printed);
        }
        if (total_open > shown) {
            std::snprintf(reply + written, reply_capacity - written, "...and %u more",
                         static_cast<unsigned>(total_open - shown));
        }
        return false;
    }

    if (startsWithCommand(body, "/clear")) {
        size_t removed = 0;
        tasks.clearCompleted(&removed);
        std::snprintf(reply, reply_capacity, "Cleared %u completed task%s.",
                     static_cast<unsigned>(removed), removed == 1 ? "" : "s");
        return removed > 0;
    }

    // The remaining commands all take one argument: a short id.
    const char* space = std::strchr(body, ' ');
    const char* arg = (space != nullptr) ? space + 1 : "";
    while (*arg == ' ') {
        ++arg;
    }

    if (startsWithCommand(body, "/done") || startsWithCommand(body, "/complete")) {
        if (arg[0] == '\0') {
            std::snprintf(reply, reply_capacity, "Usage: /done <id> — the id shown by /list.");
            return false;
        }
        const int64_t now = timeutil::systemTimeValid() ? timeutil::nowUtc() : 0;
        const bool ok = tasks.complete(arg, now) == ESP_OK;
        std::snprintf(reply, reply_capacity, ok ? "Done: %s" : "No open task with id %s", arg);
        return ok;
    }
    if (startsWithCommand(body, "/reopen") || startsWithCommand(body, "/undone")) {
        if (arg[0] == '\0') {
            std::snprintf(reply, reply_capacity, "Usage: /reopen <id>.");
            return false;
        }
        const bool ok = tasks.reopen(arg) == ESP_OK;
        std::snprintf(reply, reply_capacity, ok ? "Reopened: %s" : "No task with id %s", arg);
        return ok;
    }
    if (startsWithCommand(body, "/delete") || startsWithCommand(body, "/remove")) {
        if (arg[0] == '\0') {
            std::snprintf(reply, reply_capacity, "Usage: /delete <id>.");
            return false;
        }
        const bool ok = tasks.remove(arg) == ESP_OK;
        std::snprintf(reply, reply_capacity, ok ? "Deleted: %s" : "No task with id %s", arg);
        return ok;
    }

    std::snprintf(reply, reply_capacity,
                 "Not sure what that means. Type a task to add it, or /list, /done <id>, "
                 "/reopen <id>, /delete <id>, /clear.");
    return false;
}

void TelegramService::sendReply(const char* token, int64_t chat_id, const char* text,
                                char* buffer, size_t capacity) {
    // Both scratch buffers come from PSRAM, not this worker's stack — a 1536-byte reply's worst
    // case percent-encoding (~4.6 KB) plus the assembled body (~4.7 KB) was, until this fix, the
    // single largest contributor to this task's stack requirement by a wide margin (see
    // start()'s sizing comment). ResponseBuffer already exists for exactly this "big, short-lived,
    // must not cost internal SRAM" shape — see its own header for why.
    dashboard::net::ResponseBuffer encoded_buf(4608);
    dashboard::net::ResponseBuffer body_buf(4736);
    if (!encoded_buf.valid() || !body_buf.valid()) {
        ESP_LOGW(kTag, "reply not sent: PSRAM allocation failed");
        return;
    }
    char* encoded = encoded_buf.data();
    percentEncode(text, encoded, encoded_buf.capacity());

    char* body = body_buf.data();
    std::snprintf(body, body_buf.capacity(), "chat_id=%lld&text=%s",
                 static_cast<long long>(chat_id), encoded);

    char url[320];  // host + /bot<token up to 256> + /sendMessage
    std::snprintf(url, sizeof(url), "%s/bot%s/sendMessage", kHost, token);

    dashboard::net::HttpRequest request;
    request.url = url;
    request.post_body = body;
    request.path_is_sensitive = true;  // same reason as getUpdates — the token is in this URL

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);
    std::memset(url, 0, sizeof(url));

    if (err != ESP_OK) {
        // The command itself already happened (TaskStore was already updated by the caller) —
        // only the confirmation failed to reach the chat. Logged, not retried: retrying a
        // sendMessage after the fact risks a duplicate confirmation more than it helps.
        ESP_LOGW(kTag, "reply failed to send: HTTP %d", response.status);
    }
}

}  // namespace plugins
