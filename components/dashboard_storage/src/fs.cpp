#include "dashboard/storage/fs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"

#include "app_config.hpp"

namespace dashboard::storage {
namespace {

constexpr const char* kTag = "fs";
bool g_mounted = false;

/// Longest path we will build, including the ".tmp" suffix used by writeAtomic().
constexpr size_t kMaxPath = 128;

}  // namespace

esp_err_t Fs::mount() {
    if (g_mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = dash::cfg::kLittleFsMountPoint;
    conf.partition_label = dash::cfg::kLittleFsPartitionLabel;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        // Nothing recoverable left to try: format-on-failure was already requested, so this
        // means the partition itself is unusable.
        ESP_LOGE(kTag, "LittleFS mount failed: %s — tasks and cache will be unavailable",
                 esp_err_to_name(err));
        return err;
    }

    g_mounted = true;

    size_t total = 0;
    size_t used = 0;
    usage(&total, &used);
    ESP_LOGI(kTag, "LittleFS mounted at %s: %u KB total, %u KB used (%d%% free)",
             dash::cfg::kLittleFsMountPoint, static_cast<unsigned>(total / 1024),
             static_cast<unsigned>(used / 1024), freePercent());
    return ESP_OK;
}

bool Fs::mounted() { return g_mounted; }

void Fs::usage(size_t* total_bytes, size_t* used_bytes) {
    size_t total = 0;
    size_t used = 0;
    if (g_mounted) {
        if (esp_littlefs_info(dash::cfg::kLittleFsPartitionLabel, &total, &used) != ESP_OK) {
            total = 0;
            used = 0;
        }
    }
    if (total_bytes != nullptr) {
        *total_bytes = total;
    }
    if (used_bytes != nullptr) {
        *used_bytes = used;
    }
}

int Fs::freePercent() {
    size_t total = 0;
    size_t used = 0;
    usage(&total, &used);
    if (total == 0) {
        return 0;
    }
    if (used > total) {
        used = total;
    }
    return static_cast<int>(((total - used) * 100) / total);
}

esp_err_t Fs::writeAtomic(const char* path, const void* data, size_t len) {
    if (!g_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == nullptr || (data == nullptr && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > kMaxFileBytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Refuse before starting rather than filling the partition halfway through. The temporary
    // file needs room alongside the original, so this has to be checked with the whole write in
    // mind, not just the delta.
    const int free_percent = freePercent();
    if (free_percent < kMinFreePercentForWrite) {
        ESP_LOGE(kTag, "refusing to write %s: only %d%% free (minimum %d%%)", path, free_percent,
                 kMinFreePercentForWrite);
        return ESP_ERR_NO_MEM;
    }

    char tmp_path[kMaxPath];
    if (std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >=
        static_cast<int>(sizeof(tmp_path))) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE* file = std::fopen(tmp_path, "wb");
    if (file == nullptr) {
        ESP_LOGE(kTag, "could not open %s: %s", tmp_path, std::strerror(errno));
        return ESP_FAIL;
    }

    bool ok = true;
    if (len > 0 && std::fwrite(data, 1, len, file) != len) {
        ESP_LOGE(kTag, "short write to %s: %s", tmp_path, std::strerror(errno));
        ok = false;
    }

    // Flush all the way down before renaming. Without this the rename can land before the data,
    // which on a power loss produces exactly the atomically-wrong result this function exists to
    // prevent: a complete-looking file with incomplete contents.
    if (ok && std::fflush(file) != 0) {
        ESP_LOGE(kTag, "fflush failed for %s: %s", tmp_path, std::strerror(errno));
        ok = false;
    }
    if (ok && fsync(fileno(file)) != 0) {
        ESP_LOGE(kTag, "fsync failed for %s: %s", tmp_path, std::strerror(errno));
        ok = false;
    }
    std::fclose(file);

    if (!ok) {
        std::remove(tmp_path);
        return ESP_FAIL;
    }

    // POSIX rename() is atomic and replaces the destination. LittleFS implements it as an
    // atomic metadata update, which is the property the whole function rests on.
    if (std::rename(tmp_path, path) != 0) {
        ESP_LOGE(kTag, "rename %s -> %s failed: %s", tmp_path, path, std::strerror(errno));
        std::remove(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t Fs::readAll(const char* path, void* out, size_t capacity, size_t* out_len) {
    if (out_len != nullptr) {
        *out_len = 0;
    }
    if (!g_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (path == nullptr || out == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat info = {};
    if (stat(path, &info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t size = static_cast<size_t>(info.st_size);
    if (size > kMaxFileBytes || size > capacity) {
        ESP_LOGE(kTag, "%s is %u bytes, too large for a %u byte buffer", path,
                 static_cast<unsigned>(size), static_cast<unsigned>(capacity));
        return ESP_ERR_INVALID_SIZE;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t read = std::fread(out, 1, size, file);
    std::fclose(file);

    if (read != size) {
        ESP_LOGE(kTag, "short read from %s (%u of %u bytes)", path, static_cast<unsigned>(read),
                 static_cast<unsigned>(size));
        return ESP_FAIL;
    }
    if (out_len != nullptr) {
        *out_len = read;
    }
    return ESP_OK;
}

bool Fs::exists(const char* path) {
    if (!g_mounted || path == nullptr) {
        return false;
    }
    struct stat info = {};
    return stat(path, &info) == 0;
}

esp_err_t Fs::remove(const char* path) {
    if (!g_mounted || path == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (std::remove(path) != 0) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t Fs::ensureDirectory(const char* path) {
    if (!g_mounted || path == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(path, 0777) == 0) {
        return ESP_OK;
    }
    return errno == EEXIST ? ESP_OK : ESP_FAIL;
}

}  // namespace dashboard::storage
