// Credentials, kept apart from everything else.
//
// WHY A SEPARATE STORE AND NAMESPACE
//
//   * Enabling CONFIG_NVS_ENCRYPTION protects exactly the namespace that needs it, without
//     paying for encryption on every brightness change.
//   * Settings can be logged, dumped into the setup portal or serialised for debugging with no
//     risk of a credential coming along for the ride. That is a property of the type system
//     here, not of anyone remembering to be careful.
//   * A factory reset can wipe credentials while keeping (or separately keeping) configuration.
//
// RULES, which the API is shaped to enforce:
//
//   * Values are NEVER logged. Use describe() when you need to say something about a secret —
//     it reports presence and length only, never content.
//   * Values are fetched on demand and not held resident. Callers should zero their buffer
//     after use; scopedZero() below does it for you.
//   * The lock-screen PIN is never stored at all — only a salted SHA-256 of it.

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace dashboard::storage {

enum class Secret : uint8_t {
    WifiPassword,
    TelegramBotToken,
    ClaudeCredential,  ///< Session cookie or key. See docs/CLAUDE_USAGE.md.
    TflAppKey,         ///< Optional; TfL works unauthenticated at low rates.

    /// GitHub personal access token. Strictly optional, and the GitHub page is close to useless
    /// without it: unauthenticated the API shows only PUBLIC repos owned by the user and allows
    /// 60 requests an hour, where one refresh of ten repos costs eleven. A token lifts that to
    /// 5000/hr and is the only way to see private or organisation repositories at all.
    GithubToken,

    /// api-ninjas key for the clock page's quote. REQUIRED for that feature — the endpoint
    /// answers `{"error": "Missing API Key."}` with no key, so there is no free anonymous tier
    /// to fall back to.
    QuoteApiKey,
};

/// Longest secret this store will accept. Telegram bot tokens are ~46 characters and a Claude
/// session cookie is the longest realistic value; 256 leaves generous headroom without making
/// callers allocate anything awkward on the stack.
constexpr size_t kMaxSecretLength = 256;

class SecretStore {
  public:
    /// Store a secret. A null or empty value ERASES the key rather than storing an empty string,
    /// so "cleared" and "set to nothing" cannot drift apart.
    static esp_err_t set(Secret secret, const char* value);

    /// True if the secret is present and non-empty.
    static bool has(Secret secret);

    /// Copy the secret into `out`. Returns ESP_ERR_NOT_FOUND if unset.
    /// Zero the buffer when finished — or use ScopedSecret.
    static esp_err_t get(Secret secret, char* out, size_t capacity);

    /// Render a SAFE description for logs and the settings UI: "not set", or "set (46 chars)".
    /// Never includes any part of the value.
    static void describe(Secret secret, char* out, size_t capacity);

    /// Erase every secret. Used by factory reset.
    static esp_err_t eraseAll();

    // ---- lock screen PIN --------------------------------------------------------------
    //
    // The PIN is never stored. A random salt is generated on set, and only
    // SHA-256(salt || pin) is kept. Verification re-hashes and compares in constant time.
    //
    // This is a deterrent against someone reading your to-do list, not security: anyone with
    // physical access and a USB cable can reflash the device. Documented as such.

    static constexpr size_t kMinPinLength = 4;
    static constexpr size_t kMaxPinLength = 8;

    /// Hash and store a PIN. A null or empty PIN clears it, which also disables locking —
    /// the lock deliberately fails OPEN so a half-finished settings change cannot lock the
    /// user out of the only interface the device has.
    static esp_err_t setLockPin(const char* pin);

    static bool hasLockPin();

    /// Constant-time comparison against the stored hash. False if no PIN is set.
    static bool verifyLockPin(const char* pin);

    static esp_err_t clearLockPin();
};

/// RAII buffer that zeroes itself on destruction, so a secret does not linger on the stack.
class ScopedSecret {
  public:
    ScopedSecret() { buffer_[0] = '\0'; }
    ~ScopedSecret();

    ScopedSecret(const ScopedSecret&) = delete;
    ScopedSecret& operator=(const ScopedSecret&) = delete;

    /// Loads the secret. Returns false if absent.
    bool load(Secret secret);

    const char* c_str() const { return buffer_; }
    char* data() { return buffer_; }
    static constexpr size_t capacity() { return kMaxSecretLength; }
    bool empty() const { return buffer_[0] == '\0'; }

  private:
    char buffer_[kMaxSecretLength];
};

}  // namespace dashboard::storage
