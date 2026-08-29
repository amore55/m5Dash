// Capturing tasks from Telegram: long-poll, parse, apply to TaskStore, reply.
//
// WHY THIS IS A SERVICE WITH ITS OWN TASK, NOT A PLUGIN'S fetch()
//
// Telegram's getUpdates is a LONG poll: the request itself blocks on Telegram's server for up to
// dash::cfg::kTelegramLongPollSeconds, and the correct client behaviour is to call it again
// immediately the moment it returns — a tight, continuous loop. That does not fit
// PluginBase's model, where fetch() runs on a SCHEDULE decided by refreshIntervalMs() and the
// LVGL-thread timer that drives it. So this owns its own dashboard::Worker running ONE job that
// never returns — a plain loop, not the queue-of-discrete-jobs the class was written for
// elsewhere. TasksPlugin's own refresh cycle (dash::cfg::kTodosRefreshMs) is a separate, much
// slower "re-read TaskStore and redraw" reconcile — see tasks_plugin.hpp.
//
// SIX COMMANDS, matching Task::shortId()'s own doc comment about /done and /delete:
//
//   plain text     -> a new task, titled with the message text
//   /done <id>     -> mark complete (short id, e.g. "4821" — see Task::shortId())
//   /reopen <id>   -> mark open again
//   /delete <id>   -> remove
//   /list          -> reply with the current open tasks
//   /clear         -> remove every completed task
//
// Every command gets a reply, because this is a chat interface and a command that appears to do
// nothing is indistinguishable from one that failed.
//
// ALLOW-LISTED BY Settings::telegram_allowed_user_id. A message from anyone else still advances
// the update offset (so the bot does not get stuck retrying it) but is otherwise ignored — no
// task created, no reply sent, so a stranger who finds the bot's username learns nothing about
// whether it is even listening.
//
// THE BOT TOKEN IS IN THE URL PATH, not a header or query parameter — Telegram's own API
// convention is `https://api.telegram.org/bot<TOKEN>/<method>`. HttpRequest::path_is_sensitive
// exists because of this file: the ordinary "log everything up to the query string" rule would
// print the token straight into the log.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

#include "esp_err.h"

#include "dashboard/net/https_client.hpp"
#include "dashboard/storage/task_store.hpp"
#include "dashboard/worker.hpp"

namespace plugins {

class TelegramService {
  public:
    /// Starts the worker task and its poll loop. Call once, during app start-up. The loop then
    /// runs for the lifetime of the device — see the header for why that is the right shape here,
    /// the same as the web server never stopping.
    esp_err_t start(dashboard::storage::TaskStore& tasks);

    /// Applied live from Settings. 0 means "not configured": the loop keeps running (so a value
    /// set later takes effect without a restart) but treats every message as unauthorised.
    void setAllowedUserId(int64_t user_id) { allowed_user_id_.store(user_id); }

    /// Called after a command changes TaskStore, so the page can redraw promptly instead of
    /// waiting for its own slow reconcile interval. Runs on THIS service's worker thread — the
    /// callback must tolerate that, exactly like every other cross-thread callback in this
    /// project (it should do no more than an atomic flag set; see PluginBase::markDirty()).
    void setOnChanged(std::function<void()> callback) { on_changed_ = std::move(callback); }

  private:
    void pollLoop(dashboard::storage::TaskStore* tasks);
    esp_err_t pollOnce(dashboard::storage::TaskStore& tasks, char* buffer, size_t capacity);

    /// One update: apply it to `tasks` if the sender is allow-listed, and always fill `reply`
    /// with the text to send back (empty means "send nothing" — used for an unauthorised
    /// sender). Returns whether TaskStore was actually mutated. `text` is trimmed in place.
    bool handleMessage(dashboard::storage::TaskStore& tasks, int64_t from_id, char* text,
                       int64_t update_id, char* reply, size_t reply_capacity);

    void sendReply(const char* token, int64_t chat_id, const char* text, char* buffer,
                  size_t capacity);

    dashboard::net::HttpsClient http_;
    dashboard::Worker worker_;

    std::atomic<int64_t> allowed_user_id_{0};
    std::function<void()> on_changed_;

    /// The next getUpdates offset, persisted via CacheStore under kOffsetCacheKey — see the .cpp.
    /// Every command this bot understands is idempotent against TaskStore (upsert-by-id, and
    /// marking an already-done task done again is a no-op), so replaying old updates after a
    /// reboot would not corrupt anything even without this; persisting it just avoids the sender
    /// seeing old confirmation replies resurface after a restart.
    int64_t next_update_id_ = 0;
};

}  // namespace plugins
