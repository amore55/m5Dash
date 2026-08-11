#include "dashboard/storage/secret_store.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "app_config.hpp"

namespace dashboard::storage {
namespace {

constexpr const char* kTag = "secrets";

constexpr const char* kKeyWifiPassword = "wifi_pw";
constexpr const char* kKeyTelegramToken = "tg_token";
constexpr const char* kKeyClaudeCred = "claude_cred";
constexpr const char* kKeyTflAppKey = "tfl_key";
constexpr const char* kKeyPinHash = "pin_hash";
constexpr const char* kKeyPinSalt = "pin_salt";

constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;  // SHA-256

const char* keyFor(Secret secret) {
    switch (secret) {
        case Secret::WifiPassword:
            return kKeyWifiPassword;
        case Secret::TelegramBotToken:
            return kKeyTelegramToken;
        case Secret::ClaudeCredential:
            return kKeyClaudeCred;
        case Secret::TflAppKey:
            return kKeyTflAppKey;
    }
    return nullptr;
}

/// Overwrite a buffer before it goes out of scope.
///
/// `volatile` so the compiler cannot decide the writes are dead stores and remove them, which
/// is exactly what an optimiser does to a plain memset of a soon-to-be-discarded buffer.
void secureZero(void* data, size_t len) {
    auto* p = static_cast<volatile uint8_t*>(data);
    while (len-- > 0) {
        *p++ = 0;
    }
}

esp_err_t openNamespace(nvs_open_mode_t mode, nvs_handle_t* out) {
    return nvs_open(dash::cfg::kNvsSecretNamespace, mode, out);
}

/// SHA-256(salt || pin).
void hashPin(const uint8_t* salt, const char* pin, uint8_t out[kHashBytes]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256, not SHA-224
    mbedtls_sha256_update(&ctx, salt, kSaltBytes);
    mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(pin), std::strlen(pin));
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/// Compare in time independent of where the first difference is, so the comparison itself does
/// not leak how much of a guess was correct.
bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}  // namespace

esp_err_t SecretStore::set(Secret secret, const char* value) {
    const char* key = keyFor(secret);
    if (key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value != nullptr && std::strlen(value) >= kMaxSecretLength) {
        ESP_LOGE(kTag, "value for '%s' is too long", key);
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = openNamespace(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (value == nullptr || value[0] == '\0') {
        err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;  // already absent
        }
    } else {
        err = nvs_set_str(handle, key, value);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    // Log the KEY and the outcome, never the value.
    ESP_LOGI(kTag, "secret '%s' %s (%s)", key,
             (value == nullptr || value[0] == '\0') ? "cleared" : "stored",
             esp_err_to_name(err));
    return err;
}

bool SecretStore::has(Secret secret) {
    const char* key = keyFor(secret);
    if (key == nullptr) {
        return false;
    }
    nvs_handle_t handle = 0;
    if (openNamespace(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    const esp_err_t err = nvs_get_str(handle, key, nullptr, &len);
    nvs_close(handle);
    return err == ESP_OK && len > 1;  // len includes the NUL
}

esp_err_t SecretStore::get(Secret secret, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    const char* key = keyFor(secret);
    if (key == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = openNamespace(NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    size_t len = capacity;
    err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        out[0] = '\0';
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    return ESP_OK;
}

void SecretStore::describe(Secret secret, char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    ScopedSecret value;
    if (!value.load(secret)) {
        std::snprintf(out, capacity, "not set");
        return;
    }
    // Length only. Never a prefix, never a suffix — a partial credential is still a credential,
    // and "first four characters" is exactly what makes a leaked log useful to an attacker.
    std::snprintf(out, capacity, "set (%u chars)",
                  static_cast<unsigned>(std::strlen(value.c_str())));
}

esp_err_t SecretStore::eraseAll() {
    nvs_handle_t handle = 0;
    esp_err_t err = openNamespace(NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(kTag, "ALL secrets erased (%s)", esp_err_to_name(err));
    return err;
}

// ---------------------------------------------------------------------------------------
// Lock screen PIN
// ---------------------------------------------------------------------------------------

esp_err_t SecretStore::setLockPin(const char* pin) {
    if (pin == nullptr || pin[0] == '\0') {
        return clearLockPin();
    }
    const size_t len = std::strlen(pin);
    if (len < kMinPinLength || len > kMaxPinLength) {
        ESP_LOGE(kTag, "PIN must be %u-%u digits", static_cast<unsigned>(kMinPinLength),
                 static_cast<unsigned>(kMaxPinLength));
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t salt[kSaltBytes];
    esp_fill_random(salt, sizeof(salt));

    uint8_t hash[kHashBytes];
    hashPin(salt, pin, hash);

    nvs_handle_t handle = 0;
    esp_err_t err = openNamespace(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        secureZero(hash, sizeof(hash));
        return err;
    }
    err = nvs_set_blob(handle, kKeyPinSalt, salt, sizeof(salt));
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, kKeyPinHash, hash, sizeof(hash));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    secureZero(hash, sizeof(hash));

    ESP_LOGI(kTag, "lock PIN set (stored salted+hashed, %s)", esp_err_to_name(err));
    return err;
}

bool SecretStore::hasLockPin() {
    nvs_handle_t handle = 0;
    if (openNamespace(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t hash_len = 0;
    size_t salt_len = 0;
    const bool ok = nvs_get_blob(handle, kKeyPinHash, nullptr, &hash_len) == ESP_OK &&
                    nvs_get_blob(handle, kKeyPinSalt, nullptr, &salt_len) == ESP_OK &&
                    hash_len == kHashBytes && salt_len == kSaltBytes;
    nvs_close(handle);
    return ok;
}

bool SecretStore::verifyLockPin(const char* pin) {
    if (pin == nullptr || pin[0] == '\0') {
        return false;
    }

    nvs_handle_t handle = 0;
    if (openNamespace(NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    uint8_t salt[kSaltBytes];
    uint8_t stored[kHashBytes];
    size_t salt_len = sizeof(salt);
    size_t hash_len = sizeof(stored);
    const bool read_ok = nvs_get_blob(handle, kKeyPinSalt, salt, &salt_len) == ESP_OK &&
                         nvs_get_blob(handle, kKeyPinHash, stored, &hash_len) == ESP_OK &&
                         salt_len == kSaltBytes && hash_len == kHashBytes;
    nvs_close(handle);

    if (!read_ok) {
        return false;
    }

    uint8_t candidate[kHashBytes];
    hashPin(salt, pin, candidate);
    const bool match = constantTimeEquals(candidate, stored, kHashBytes);

    secureZero(candidate, sizeof(candidate));
    secureZero(stored, sizeof(stored));
    return match;
}

esp_err_t SecretStore::clearLockPin() {
    nvs_handle_t handle = 0;
    esp_err_t err = openNamespace(NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(handle, kKeyPinHash);
    nvs_erase_key(handle, kKeyPinSalt);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGW(kTag, "lock PIN cleared; the lock screen will now fail open");
    return err;
}

// ---------------------------------------------------------------------------------------

ScopedSecret::~ScopedSecret() { secureZero(buffer_, sizeof(buffer_)); }

bool ScopedSecret::load(Secret secret) {
    return SecretStore::get(secret, buffer_, sizeof(buffer_)) == ESP_OK && buffer_[0] != '\0';
}

}  // namespace dashboard::storage
