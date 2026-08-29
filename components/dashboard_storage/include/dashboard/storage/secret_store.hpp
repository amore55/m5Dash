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

    /// GitHub token for the user's OWN repositories.
    ///
    /// Strictly optional, and the GitHub page is thin without it: unauthenticated the API shows
    /// only PUBLIC repos and allows 60 requests an hour, where one refresh costs seven. A token
    /// lifts that to 5000/hr and is the only way to see private repositories at all.
    GithubToken,

    /// GitHub token for WORK repositories, kept separate on purpose.
    ///
    /// A fine-grained token has exactly ONE resource owner: a token owned by a personal account
    /// cannot see organisation repositories however its permissions are set. So covering both
    /// needs either one CLASSIC token — whose narrowest private-repo scope is `repo`, meaning
    /// read AND WRITE on every repository including the employer's — or two fine-grained ones.
    ///
    /// Two is the safer arrangement and is why this exists: each token grants read-only access to
    /// one owner's repositories, and neither can write anything.
    GithubWorkToken,

    /// api-ninjas key for the clock page's quote. REQUIRED for that feature — the endpoint
    /// answers `{"error": "Missing API Key."}` with no key, so there is no free anonymous tier
    /// to fall back to.
    QuoteApiKey,

    /// Microsoft Graph OAuth refresh token for the calendar page.
    ///
    /// The ONLY secret here the device itself ever chooses to overwrite: the device-code sign-in
    /// exchanges it for a fresh one, and Microsoft rotates it on every use, so a new value is
    /// stored every time the calendar refreshes rather than once at setup. See kMaxLongSecretLength
    /// — this is also the one secret long enough to need it.
    MicrosoftRefreshToken,
};

/// Longest secret this store will accept. Telegram bot tokens are ~46 characters and a Claude
/// session cookie is the longest realistic value; 256 leaves generous headroom without making
/// callers allocate anything awkward on the stack.
constexpr size_t kMaxSecretLength = 256;

/// Longest secret for the one credential that does not fit the general case: Microsoft's OAuth
/// refresh token. Public-client refresh tokens from the v2.0 endpoint are long opaque blobs,
/// commonly a thousand characters or more, with no vendor-documented firm maximum — unlike every
/// other secret here, which is a short human-issued key. Sized with real headroom rather than
/// measured, because guessing too small fails silently in the worst place: SecretStore::set()
/// would refuse to save an otherwise-successful sign-in, and the symptom would be "it signed in
/// and then immediately asked again."
constexpr size_t kMaxLongSecretLength = 2048;

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
