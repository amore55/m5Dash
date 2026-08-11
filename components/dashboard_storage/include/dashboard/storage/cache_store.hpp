// Last-known-good API responses, on disk.
//
// This is what makes the Stale state in PluginBase meaningful. Without it, a device that reboots
// while the internet is down shows five empty pages; with it, it shows yesterday's weather
// clearly labelled as old. The brief asks for exactly that behaviour, and "cache the last
// successful response" is only useful if the cache survives a power cut.
//
// Entries are stored as individual files under /store/cache/, one per key, written atomically.
// One file per key rather than a single combined document so that a corrupt weather response
// cannot take the TfL cache down with it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "esp_err.h"

namespace dashboard::storage {

class CacheStore {
  public:
    /// Create the cache directory. Safe to call repeatedly.
    static esp_err_t init();

    /// Store a response under `key`. Keys must be short and filename-safe (plugin ids are).
    /// Larger than dash::cfg::kMaxCacheEntryBytes is rejected rather than truncated — a
    /// truncated cache entry is worse than no cache entry, because it parses as corrupt data
    /// on the next boot instead of simply being absent.
    static esp_err_t put(const char* key, const void* data, size_t len);

    /// Read a cached response. `age_seconds`, if provided, receives how old it is — which is
    /// what the UI needs in order to say "data from 3 hours ago" rather than just "stale".
    /// Returns ESP_ERR_NOT_FOUND if there is no entry.
    static esp_err_t get(const char* key, void* out, size_t capacity, size_t* out_len,
                         int64_t* age_seconds = nullptr);

    /// When the entry was written, as epoch seconds. 0 if absent or the clock was unset at the
    /// time of writing.
    static int64_t timestamp(const char* key);

    static bool has(const char* key);
    static esp_err_t remove(const char* key);

    /// Drop every cached entry. Part of factory reset.
    static esp_err_t clearAll();

  private:
    /// Build "/store/cache/<key>.json". False if the key is unsafe or the path is too long.
    static bool buildPath(const char* key, char* out, size_t capacity);
};

}  // namespace dashboard::storage
