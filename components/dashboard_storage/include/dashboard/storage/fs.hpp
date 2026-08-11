// LittleFS mount and crash-safe file replacement.
//
// WHY LITTLEFS AND NOT SPIFFS
//
// The brief requires surviving power loss during a write. SPIFFS does not — an interrupted
// write can leave the filesystem itself inconsistent. LittleFS is designed around power-fail
// safety: its metadata updates are atomic and it recovers from an interrupted write on mount.
//
// WHY writeAtomic() ON TOP OF THAT
//
// LittleFS guarantees the FILESYSTEM stays consistent. It does not guarantee that YOUR file
// contains either the old contents or the new ones — a crash midway through overwriting
// tasks.json legitimately leaves a half-written task list, which parses as either garbage or,
// worse, a plausible-but-truncated list. writeAtomic() closes that gap with the standard
// write-to-temporary-then-rename dance, so a reader only ever sees a complete version.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace dashboard::storage {

class Fs {
  public:
    /// Mount the LittleFS partition, formatting it if it cannot be mounted.
    ///
    /// Formatting on failure is deliberate: an unmountable data partition on a headless desk
    /// device is otherwise unrecoverable without a USB cable, and the contents (tasks, cached
    /// API responses) are all re-creatable. Configuration lives in NVS and is unaffected.
    static esp_err_t mount();

    static bool mounted();

    /// Bytes total / used on the partition. Both zero if unmounted.
    static void usage(size_t* total_bytes, size_t* used_bytes);

    /// Free space as a percentage, 0-100. Used to refuse writes before the filesystem fills.
    static int freePercent();

    /// Replace a file's contents atomically.
    ///
    /// Writes to "<path>.tmp", flushes it to storage, then renames over the target. A reader
    /// therefore sees either the previous complete version or the new complete version, never a
    /// partial one — and a crash at any point leaves at worst a stray .tmp file, which the next
    /// write overwrites.
    ///
    /// Refuses to write when free space is below kMinFreePercentForWrite, so filling the
    /// filesystem degrades into a clear error rather than a corrupt store.
    static esp_err_t writeAtomic(const char* path, const void* data, size_t len);

    /// Read a whole file into `out`. `out_len` receives the byte count.
    /// Returns ESP_ERR_NOT_FOUND if absent, ESP_ERR_INVALID_SIZE if it does not fit.
    static esp_err_t readAll(const char* path, void* out, size_t capacity, size_t* out_len);

    static bool exists(const char* path);
    static esp_err_t remove(const char* path);

    /// Create a directory, tolerating "already exists".
    static esp_err_t ensureDirectory(const char* path);

    /// Below this, writeAtomic() refuses. A write needs room for the temporary copy as well as
    /// the original, so the threshold has to leave headroom for both.
    static constexpr int kMinFreePercentForWrite = 10;

    /// Ceiling on readAll(), independent of the caller's buffer — a corrupted length field
    /// should not be able to ask for an enormous allocation.
    static constexpr size_t kMaxFileBytes = 128u * 1024u;
};

}  // namespace dashboard::storage
