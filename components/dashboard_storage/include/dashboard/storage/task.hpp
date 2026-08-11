// The task record, as specified in the brief.
//
// Fixed-size throughout: a task list is user-supplied data arriving over the network, and a
// bounded record means a long title truncates instead of allocating.

#pragma once

#include <cstdint>

#include "dashboard/fixed_string.hpp"

namespace dashboard::storage {

enum class TaskStatus : uint8_t {
    Open,
    Done,
};

enum class TaskPriority : uint8_t {
    Normal,
    High,
    Low,
};

enum class TaskSource : uint8_t {
    Telegram,  ///< Captured from a Telegram message.
    Device,    ///< Typed on the Tab5 itself.
};

const char* toString(TaskStatus value);
const char* toString(TaskPriority value);
const char* toString(TaskSource value);
TaskStatus taskStatusFromString(const char* text);
TaskPriority taskPriorityFromString(const char* text);
TaskSource taskSourceFromString(const char* text);

struct Task {
    /// Stable identifier, also the deduplication key.
    ///
    /// For Telegram-sourced tasks this is derived from the message's `update_id`
    /// ("tg-<update_id>"), which makes re-processing an update IDEMPOTENT: replaying the same
    /// message after a restart upserts the same record instead of creating a duplicate. That is
    /// what makes duplicate prevention robust even if the device dies between creating a task
    /// and persisting the last-seen update id.
    FixedString<24> id;

    TextString title;
    TaskStatus status = TaskStatus::Open;
    TaskPriority priority = TaskPriority::Normal;
    ShortString category;

    /// Unix epoch seconds. 0 means "not set" for due_at and completed_at.
    int64_t created_at = 0;
    int64_t due_at = 0;
    int64_t completed_at = 0;

    TaskSource source = TaskSource::Telegram;

    bool open() const { return status == TaskStatus::Open; }

    /// Short display id for the Telegram `/done <id>` and `/delete <id>` commands.
    /// Users should not have to type "tg-123456789", so the trailing digits are used.
    void shortId(char* out, size_t capacity) const;
};

}  // namespace dashboard::storage
