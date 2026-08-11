// The device's task list: bounded, persisted, crash-safe.
//
// STORAGE MODEL
//
// A single JSON document rewritten atomically via Fs::writeAtomic(). The brief warns against
// "repeatedly rewriting large JSON blobs into NVS" — the objection is to NVS, whose wear
// levelling and size limits make that a genuinely bad idea. On LittleFS, with the list capped
// at dash::cfg::kMaxTasks and writes happening only on user action or an incoming message, a
// whole-file rewrite is a few tens of kilobytes at human timescales. That is a fair trade for a
// format that is trivially inspectable and cannot end up half-applied.
//
// If the task list ever grows to the point where that stops being true, the answer is an
// append-only journal with periodic compaction — not moving it into NVS.
//
// THREADING
//
// Touched by the Telegram worker task and by the UI thread, so every public method takes an
// internal mutex. Reads copy out rather than handing back pointers into the array, because a
// pointer would outlive the lock.

#pragma once

#include <array>
#include <cstddef>
#include <mutex>

#include "esp_err.h"

#include "app_config.hpp"
#include "dashboard/storage/task.hpp"

namespace dashboard::storage {

class TaskStore {
  public:
    /// Load from disk. A missing file is not an error — it means an empty list.
    ///
    /// A file that fails to parse is reported, and the store starts empty rather than refusing
    /// to run. The old file is left on disk untouched so it can be recovered manually; silently
    /// overwriting someone's task list because one byte was wrong would be worse than losing
    /// the session.
    esp_err_t load();

    /// Insert or update by id, then persist.
    ///
    /// Upsert rather than insert is what makes Telegram replay safe: the same `update_id`
    /// always produces the same task, however many times it is processed.
    /// Returns ESP_ERR_NO_MEM when the list is full — deliberately a loud failure rather than
    /// silently discarding the oldest task.
    esp_err_t upsert(const Task& task);

    /// Mark complete. `when` is the completion timestamp (epoch seconds).
    esp_err_t complete(const char* id, int64_t when);

    /// Reopen a completed task.
    esp_err_t reopen(const char* id);

    esp_err_t remove(const char* id);

    /// Delete every completed task. Returns the number removed via `removed` if non-null.
    esp_err_t clearCompleted(size_t* removed = nullptr);

    size_t size() const;
    size_t openCount() const;
    bool full() const;

    /// Copy the task at `index` (in stored order). False if out of range.
    bool at(size_t index, Task& out) const;

    /// Copy the task matching `id`, or a task whose short id matches. False if not found.
    bool find(const char* id, Task& out) const;

    /// Copy up to `capacity` tasks, optionally only open ones. Returns how many were written.
    size_t snapshot(Task* out, size_t capacity, bool open_only) const;

    /// Path used on the filesystem. Exposed for diagnostics and tests.
    static const char* path();

  private:
    esp_err_t saveLocked();
    int findIndexLocked(const char* id) const;
    /// Caller must hold mutex_. std::mutex is not recursive, so the locking and non-locking
    /// forms are kept explicitly separate rather than relying on callers to remember.
    size_t openCountLocked() const;

    mutable std::mutex mutex_;
    std::array<Task, dash::cfg::kMaxTasks> tasks_{};
    size_t count_ = 0;
};

}  // namespace dashboard::storage
