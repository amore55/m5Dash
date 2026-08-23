#include "dashboard/storage/settings_store.hpp"

#include <cinttypes>
#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.hpp"
#include "dashboard/storage/settings_migrate.hpp"

namespace dashboard::storage {
namespace {

constexpr const char* kTag = "settings";

// NVS keys are limited to 15 characters, which is why these are abbreviated rather than
// spelled out. Once written to a device they are effectively permanent: renaming one requires a
// migration, so treat this list as an on-disk format, not an implementation detail.
constexpr const char* kKeySchema = "schema";
constexpr const char* kKeyDeviceName = "dev_name";
constexpr const char* kKeyTimezone = "tz";
constexpr const char* kKeyWifiSsid = "wifi_ssid";
constexpr const char* kKeyBrightness = "bright";
constexpr const char* kKeyNightBrightness = "night_bright";
constexpr const char* kKeyDimStart = "dim_start";
constexpr const char* kKeyDimEnd = "dim_end";
constexpr const char* kKeyClockStyle = "clock_style";
constexpr const char* kKeyShowSeconds = "show_secs";
constexpr const char* kKeyLockEnabled = "lock_en";
constexpr const char* kKeyLockIdle = "lock_idle";
constexpr const char* kKeyLockWeather = "lock_wx";
constexpr const char* kKeyWallpaperStyle = "wall_style";
constexpr const char* kKeyWeatherLabel = "wx_label";
constexpr const char* kKeyLatitude = "wx_lat";
constexpr const char* kKeyLongitude = "wx_lon";
constexpr const char* kKeyCommuteAmStart = "cm_am_start";
constexpr const char* kKeyCommuteAmEnd = "cm_am_end";
constexpr const char* kKeyCommutePmStart = "cm_pm_start";
constexpr const char* kKeyCommutePmEnd = "cm_pm_end";
constexpr const char* kKeyTelegramUser = "tg_user";
constexpr const char* kKeyClaudeProvider = "claude_prov";
constexpr const char* kKeyClaudeOrg = "claude_org";
constexpr const char* kKeyClaudeRelay = "claude_relay";
constexpr const char* kKeyDisplayFlipped = "disp_flip";
constexpr const char* kKeyGithubUser = "gh_user";
constexpr const char* kKeyGithubShowWork = "gh_work";
constexpr const char* kKeyGithubOrg = "gh_org";
constexpr const char* kKeyGithubAliases = "gh_alias";
constexpr const char* kKeyDefaultPage = "def_page";
constexpr const char* kKeyPagesEnabled = "pages_en";
constexpr const char* kKeyPagesOrder = "pages_order";
constexpr const char* kKeyOtaChannel = "ota_chan";
constexpr const char* kKeyOtaUrl = "ota_url";
constexpr const char* kKeyOtaAuto = "ota_auto";

/// Latitude/longitude are stored as signed micro-degrees.
///
/// NVS has no floating-point type, and a raw double blob would be sensitive to layout and
/// endianness. int32 micro-degrees is exact, endian-safe, human-readable in an NVS dump, and
/// resolves to ~0.11 m — far finer than a weather forecast grid.
constexpr double kDegreesToMicro = 1000000.0;

int32_t degreesToMicro(double degrees) {
    return static_cast<int32_t>(std::lround(degrees * kDegreesToMicro));
}
double microToDegrees(int32_t micro) { return static_cast<double>(micro) / kDegreesToMicro; }

// ---- readers that fall back to whatever is already in the struct ------------------------
// Every getter leaves the destination untouched when the key is absent, so an unset key means
// "keep the default" rather than "zero".

template <size_t N>
void readString(nvs_handle_t handle, const char* key, FixedString<N>& out) {
    char buffer[N];
    size_t len = sizeof(buffer);
    if (nvs_get_str(handle, key, buffer, &len) == ESP_OK) {
        out.assign(buffer);
    }
}

void readI32(nvs_handle_t handle, const char* key, int32_t& out) {
    int32_t value = 0;
    if (nvs_get_i32(handle, key, &value) == ESP_OK) {
        out = value;
    }
}

void readI64(nvs_handle_t handle, const char* key, int64_t& out) {
    int64_t value = 0;
    if (nvs_get_i64(handle, key, &value) == ESP_OK) {
        out = value;
    }
}

void readBool(nvs_handle_t handle, const char* key, bool& out) {
    uint8_t value = 0;
    if (nvs_get_u8(handle, key, &value) == ESP_OK) {
        out = (value != 0);
    }
}

void readU32(nvs_handle_t handle, const char* key, uint32_t& out) {
    uint32_t value = 0;
    if (nvs_get_u32(handle, key, &value) == ESP_OK) {
        out = value;
    }
}

/// Record the first error but keep going, so one bad key does not abandon the rest of the save.
void note(esp_err_t& first, esp_err_t err, const char* key) {
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "failed to write '%s': %s", key, esp_err_to_name(err));
        if (first == ESP_OK) {
            first = err;
        }
    }
}

}  // namespace

esp_err_t SettingsStore::load(Settings& out) {
    Settings settings;  // starts at defaults

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(dash::cfg::kNvsConfigNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Fresh device: the namespace does not exist yet. Defaults are exactly right, but they
        // are also written back immediately.
        //
        // Persisting on first boot stamps the schema version, so the 0 -> 1 migration does not
        // re-run on every subsequent start, and it makes the on-disk state explicit rather than
        // "absent, therefore assumed". It also means the first genuine settings change is a
        // modification of an existing namespace rather than a creation, which is one fewer
        // failure mode to reason about at the point where a user is actually waiting.
        ESP_LOGI(kTag, "no stored configuration; writing defaults");
        settings.clampToValidRanges();
        out = settings;
        const esp_err_t write_err = save(settings);
        if (write_err != ESP_OK) {
            ESP_LOGW(kTag, "could not persist initial defaults: %s", esp_err_to_name(write_err));
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed: %s; falling back to defaults", esp_err_to_name(err));
        settings.clampToValidRanges();
        out = settings;
        return ESP_OK;  // deliberately not fatal — see the header
    }

    uint32_t stored_schema = 0;
    readU32(handle, kKeySchema, stored_schema);

    readString(handle, kKeyDeviceName, settings.device_name);
    readString(handle, kKeyTimezone, settings.timezone);
    readString(handle, kKeyWifiSsid, settings.wifi_ssid);

    readI32(handle, kKeyBrightness, settings.brightness_percent);
    readI32(handle, kKeyNightBrightness, settings.night_brightness_percent);
    readI32(handle, kKeyDimStart, settings.dim_start_minutes);
    readI32(handle, kKeyDimEnd, settings.dim_end_minutes);
    readString(handle, kKeyClockStyle, settings.clock_style);
    readBool(handle, kKeyShowSeconds, settings.show_seconds);

    readBool(handle, kKeyLockEnabled, settings.lock_enabled);
    readI32(handle, kKeyLockIdle, settings.lock_idle_timeout_minutes);
    readBool(handle, kKeyLockWeather, settings.lock_show_weather);
    readString(handle, kKeyWallpaperStyle, settings.wallpaper_style);

    readString(handle, kKeyWeatherLabel, settings.weather_label);
    int32_t lat_micro = degreesToMicro(settings.latitude);
    int32_t lon_micro = degreesToMicro(settings.longitude);
    readI32(handle, kKeyLatitude, lat_micro);
    readI32(handle, kKeyLongitude, lon_micro);
    settings.latitude = microToDegrees(lat_micro);
    settings.longitude = microToDegrees(lon_micro);

    readI32(handle, kKeyCommuteAmStart, settings.commute_morning_start_minutes);
    readI32(handle, kKeyCommuteAmEnd, settings.commute_morning_end_minutes);
    readI32(handle, kKeyCommutePmStart, settings.commute_evening_start_minutes);
    readI32(handle, kKeyCommutePmEnd, settings.commute_evening_end_minutes);

    readI64(handle, kKeyTelegramUser, settings.telegram_allowed_user_id);

    ShortString provider{toString(settings.claude_provider)};
    readString(handle, kKeyClaudeProvider, provider);
    settings.claude_provider = claudeProviderFromString(provider.c_str());
    readString(handle, kKeyClaudeOrg, settings.claude_organisation_id);
    readString(handle, kKeyClaudeRelay, settings.claude_relay_url);

    readBool(handle, kKeyDisplayFlipped, settings.display_flipped);
    readString(handle, kKeyGithubUser, settings.github_username);
    readBool(handle, kKeyGithubShowWork, settings.github_show_work);
    readString(handle, kKeyGithubOrg, settings.github_organisation);
    readString(handle, kKeyGithubAliases, settings.github_aliases);

    readString(handle, kKeyDefaultPage, settings.default_page);
    readString(handle, kKeyPagesEnabled, settings.enabled_pages);
    readString(handle, kKeyPagesOrder, settings.page_order);

    readString(handle, kKeyOtaChannel, settings.ota_channel);
    readString(handle, kKeyOtaUrl, settings.ota_manifest_url);
    readBool(handle, kKeyOtaAuto, settings.ota_automatic_install);

    nvs_close(handle);

    const MigrationResult migration = migrateSettings(settings, stored_schema);
    if (migration.downgraded) {
        ESP_LOGW(kTag,
                 "stored schema %" PRIu32 " is newer than this firmware's %" PRIu32
                 " (OTA rollback?); keeping values",
                 migration.from_schema, migration.to_schema);
    } else if (migration.changed) {
        ESP_LOGI(kTag, "migrated configuration schema %" PRIu32 " -> %" PRIu32,
                 migration.from_schema, migration.to_schema);
    }

    settings.clampToValidRanges();
    out = settings;

    if (migration.changed) {
        // Persist immediately so the migration is not re-run on every boot, and so a later
        // partial write cannot leave a half-migrated configuration.
        const esp_err_t write_err = save(settings);
        if (write_err != ESP_OK) {
            ESP_LOGW(kTag, "could not persist migrated settings: %s", esp_err_to_name(write_err));
        }
    }

    ESP_LOGI(kTag, "configuration loaded (schema %" PRIu32 ", %s)", settings.schema,
             settings.provisioned() ? "provisioned" : "NOT provisioned - setup required");
    return ESP_OK;
}

esp_err_t SettingsStore::save(const Settings& settings) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(dash::cfg::kNvsConfigNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open for write failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_err_t first = ESP_OK;
    note(first, nvs_set_u32(handle, kKeySchema, Settings::kCurrentSchema), kKeySchema);

    note(first, nvs_set_str(handle, kKeyDeviceName, settings.device_name.c_str()), kKeyDeviceName);
    note(first, nvs_set_str(handle, kKeyTimezone, settings.timezone.c_str()), kKeyTimezone);
    note(first, nvs_set_str(handle, kKeyWifiSsid, settings.wifi_ssid.c_str()), kKeyWifiSsid);

    note(first, nvs_set_i32(handle, kKeyBrightness, settings.brightness_percent), kKeyBrightness);
    note(first, nvs_set_i32(handle, kKeyNightBrightness, settings.night_brightness_percent),
         kKeyNightBrightness);
    note(first, nvs_set_i32(handle, kKeyDimStart, settings.dim_start_minutes), kKeyDimStart);
    note(first, nvs_set_i32(handle, kKeyDimEnd, settings.dim_end_minutes), kKeyDimEnd);
    note(first, nvs_set_str(handle, kKeyClockStyle, settings.clock_style.c_str()), kKeyClockStyle);
    note(first, nvs_set_u8(handle, kKeyShowSeconds, settings.show_seconds ? 1 : 0),
         kKeyShowSeconds);

    note(first, nvs_set_u8(handle, kKeyLockEnabled, settings.lock_enabled ? 1 : 0),
         kKeyLockEnabled);
    note(first, nvs_set_i32(handle, kKeyLockIdle, settings.lock_idle_timeout_minutes),
         kKeyLockIdle);
    note(first, nvs_set_u8(handle, kKeyLockWeather, settings.lock_show_weather ? 1 : 0),
         kKeyLockWeather);
    note(first, nvs_set_str(handle, kKeyWallpaperStyle, settings.wallpaper_style.c_str()),
         kKeyWallpaperStyle);

    note(first, nvs_set_str(handle, kKeyWeatherLabel, settings.weather_label.c_str()),
         kKeyWeatherLabel);
    note(first, nvs_set_i32(handle, kKeyLatitude, degreesToMicro(settings.latitude)),
         kKeyLatitude);
    note(first, nvs_set_i32(handle, kKeyLongitude, degreesToMicro(settings.longitude)),
         kKeyLongitude);

    note(first, nvs_set_i32(handle, kKeyCommuteAmStart, settings.commute_morning_start_minutes),
         kKeyCommuteAmStart);
    note(first, nvs_set_i32(handle, kKeyCommuteAmEnd, settings.commute_morning_end_minutes),
         kKeyCommuteAmEnd);
    note(first, nvs_set_i32(handle, kKeyCommutePmStart, settings.commute_evening_start_minutes),
         kKeyCommutePmStart);
    note(first, nvs_set_i32(handle, kKeyCommutePmEnd, settings.commute_evening_end_minutes),
         kKeyCommutePmEnd);

    note(first, nvs_set_i64(handle, kKeyTelegramUser, settings.telegram_allowed_user_id),
         kKeyTelegramUser);

    note(first, nvs_set_str(handle, kKeyClaudeProvider, toString(settings.claude_provider)),
         kKeyClaudeProvider);
    note(first, nvs_set_str(handle, kKeyClaudeOrg, settings.claude_organisation_id.c_str()),
         kKeyClaudeOrg);
    note(first, nvs_set_str(handle, kKeyClaudeRelay, settings.claude_relay_url.c_str()),
         kKeyClaudeRelay);

    note(first, nvs_set_u8(handle, kKeyDisplayFlipped, settings.display_flipped ? 1 : 0),
         kKeyDisplayFlipped);
    note(first, nvs_set_str(handle, kKeyGithubUser, settings.github_username.c_str()),
         kKeyGithubUser);
    note(first, nvs_set_u8(handle, kKeyGithubShowWork, settings.github_show_work ? 1 : 0),
         kKeyGithubShowWork);
    note(first, nvs_set_str(handle, kKeyGithubOrg, settings.github_organisation.c_str()),
         kKeyGithubOrg);
    note(first, nvs_set_str(handle, kKeyGithubAliases, settings.github_aliases.c_str()),
         kKeyGithubAliases);

    note(first, nvs_set_str(handle, kKeyDefaultPage, settings.default_page.c_str()),
         kKeyDefaultPage);
    note(first, nvs_set_str(handle, kKeyPagesEnabled, settings.enabled_pages.c_str()),
         kKeyPagesEnabled);
    note(first, nvs_set_str(handle, kKeyPagesOrder, settings.page_order.c_str()), kKeyPagesOrder);

    note(first, nvs_set_str(handle, kKeyOtaChannel, settings.ota_channel.c_str()), kKeyOtaChannel);
    note(first, nvs_set_str(handle, kKeyOtaUrl, settings.ota_manifest_url.c_str()), kKeyOtaUrl);
    note(first, nvs_set_u8(handle, kKeyOtaAuto, settings.ota_automatic_install ? 1 : 0),
         kKeyOtaAuto);

    // One commit for the whole set. NVS commits are atomic, so a power loss here leaves the
    // previous configuration intact rather than a half-written one.
    const esp_err_t commit_err = nvs_commit(handle);
    nvs_close(handle);

    if (commit_err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_commit failed: %s", esp_err_to_name(commit_err));
        return commit_err;
    }
    if (first != ESP_OK) {
        return first;
    }
    ESP_LOGI(kTag, "configuration saved");
    return ESP_OK;
}

esp_err_t SettingsStore::erase() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(dash::cfg::kNvsConfigNamespace, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  // nothing to erase
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(kTag, "configuration erased (%s)", esp_err_to_name(err));
    return err;
}

}  // namespace dashboard::storage
