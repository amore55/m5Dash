#include "dashboard/storage/cache_store.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "app_config.hpp"
#include "dashboard/storage/fs.hpp"

namespace dashboard::storage {
namespace {

constexpr const char* kTag = "cache";
constexpr const char* kDirectory = "/store/cache";
constexpr size_t kMaxPath = 128;

/// Only plugin-id-shaped keys are allowed: letters, digits and underscore.
///
/// This is a path-traversal guard, not tidiness. A key of "../../nvs" would otherwise let a
/// caller write outside the cache directory, and cache keys are close enough to plugin-supplied
/// data that validating them here is cheaper than trusting every call site.
bool keyIsSafe(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }
    size_t len = 0;
    for (const char* p = key; *p != '\0'; ++p, ++len) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!std::isalnum(c) && c != '_') {
            return false;
        }
    }
    return len <= 32;
}

}  // namespace

bool CacheStore::buildPath(const char* key, char* out, size_t capacity) {
    if (!keyIsSafe(key)) {
        ESP_LOGE(kTag, "rejecting unsafe cache key");
        return false;
    }
    const int written = std::snprintf(out, capacity, "%s/%s.json", kDirectory, key);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

esp_err_t CacheStore::init() {
    if (!Fs::mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = Fs::ensureDirectory(kDirectory);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "could not create %s", kDirectory);
    }
    return err;
}

esp_err_t CacheStore::put(const char* key, const void* data, size_t len) {
    if (len > dash::cfg::kMaxCacheEntryBytes) {
        ESP_LOGW(kTag, "'%s' response is %u bytes, over the %u byte cache limit; not cached",
                 key, static_cast<unsigned>(len),
                 static_cast<unsigned>(dash::cfg::kMaxCacheEntryBytes));
        return ESP_ERR_INVALID_SIZE;
    }
    char path[kMaxPath];
    if (!buildPath(key, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    return Fs::writeAtomic(path, data, len);
}

esp_err_t CacheStore::get(const char* key, void* out, size_t capacity, size_t* out_len,
                          int64_t* age_seconds) {
    char path[kMaxPath];
    if (!buildPath(key, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = Fs::readAll(path, out, capacity, out_len);
    if (err != ESP_OK) {
        return err;
    }

    if (age_seconds != nullptr) {
        const int64_t written = timestamp(key);
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        // A negative age would mean the clock moved backwards — typically the first NTP sync
        // after boot correcting an RTC-restored guess. Report 0 rather than a nonsense value.
        *age_seconds = (written > 0 && now > written) ? (now - written) : 0;
    }
    return ESP_OK;
}

int64_t CacheStore::timestamp(const char* key) {
    char path[kMaxPath];
    if (!buildPath(key, path, sizeof(path))) {
        return 0;
    }
    struct stat info = {};
    if (stat(path, &info) != 0) {
        return 0;
    }
    return static_cast<int64_t>(info.st_mtime);
}

bool CacheStore::has(const char* key) {
    char path[kMaxPath];
    return buildPath(key, path, sizeof(path)) && Fs::exists(path);
}

esp_err_t CacheStore::remove(const char* key) {
    char path[kMaxPath];
    if (!buildPath(key, path, sizeof(path))) {
        return ESP_ERR_INVALID_ARG;
    }
    return Fs::remove(path);
}

esp_err_t CacheStore::clearAll() {
    if (!Fs::mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    DIR* dir = opendir(kDirectory);
    if (dir == nullptr) {
        return ESP_OK;  // nothing cached yet
    }
    size_t removed = 0;
    const struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        char path[kMaxPath];
        if (std::snprintf(path, sizeof(path), "%s/%s", kDirectory, entry->d_name) <
            static_cast<int>(sizeof(path))) {
            if (Fs::remove(path) == ESP_OK) {
                ++removed;
            }
        }
    }
    closedir(dir);
    ESP_LOGW(kTag, "cleared %u cached responses", static_cast<unsigned>(removed));
    return ESP_OK;
}

}  // namespace dashboard::storage
