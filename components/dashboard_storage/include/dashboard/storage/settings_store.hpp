// Persistence for Settings, in NVS.
//
// Values are stored as INDIVIDUAL KEYS rather than one serialised blob. That costs a little more
// code than a single `nvs_set_blob` of the struct, and buys three things worth having:
//
//   * Adding a field needs no migration — it simply reads back as its default.
//   * A single corrupted or missing entry loses one setting, not the entire configuration.
//   * The stored data survives struct layout changes, padding and compiler differences, which a
//     raw struct blob emphatically does not.
//
// Secrets are NOT here. See SecretStore.

#pragma once

#include "esp_err.h"

#include "dashboard/storage/settings.hpp"

namespace dashboard::storage {

class SettingsStore {
  public:
    /// Read everything, apply schema migrations, clamp to valid ranges.
    ///
    /// Always produces a usable Settings object. A device with an empty or unreadable NVS
    /// namespace gets defaults rather than an error — the setup portal exists precisely to
    /// recover from that, and refusing to boot would remove the only way to fix it.
    ///
    /// If a migration changed anything, the result is written straight back.
    esp_err_t load(Settings& out);

    /// Write every field and commit. Returns the first failure, having attempted the rest.
    esp_err_t save(const Settings& settings);

    /// Erase the settings namespace only. Secrets are untouched — see
    /// SecretStore::eraseAll() and factoryReset() in the storage facade for the full wipe.
    esp_err_t erase();
};

}  // namespace dashboard::storage
