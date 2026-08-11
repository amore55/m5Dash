// Configuration schema migration.
//
// The brief requires "versioned configuration schemas and support for future migration", and
// this is the mechanism. It is deliberately tiny today — there is only one schema — but the
// shape matters more than the content: getting it in now means the first real migration is a
// five-line addition rather than a redesign on a device that already holds someone's settings.
//
// WHEN YOU NEED A MIGRATION
//
// Settings are stored as INDIVIDUAL NVS KEYS, not one blob. That means:
//
//   * Adding a field           -> no migration. It reads back as its default on old devices.
//   * Deleting a field         -> no migration. The stale key is ignored and cleaned up.
//   * Renaming a field         -> MIGRATION (copy old key to new, erase old).
//   * Changing a field's units
//     or meaning               -> MIGRATION (this is the dangerous one, and the whole reason
//                                 the schema number exists — the value still *loads*, it is
//                                 just silently wrong).
//
// Only the last two need a step here, and the second is why a version number is not optional:
// nothing else can detect it.
//
// Host-testable: no ESP-IDF dependency.

#pragma once

#include <cstdint>

#include "dashboard/storage/settings.hpp"

namespace dashboard::storage {

/// Outcome of a migration attempt, so the caller can log something useful and decide whether
/// to write the result back.
struct MigrationResult {
    bool changed = false;      ///< Settings were modified and should be persisted.
    bool downgraded = false;   ///< Stored schema was NEWER than this firmware understands.
    uint32_t from_schema = 0;
    uint32_t to_schema = 0;
};

/// Bring `settings` up to Settings::kCurrentSchema, in place.
///
/// `stored_schema` is the version read from NVS (0 if absent, i.e. a fresh device).
///
/// A stored schema NEWER than this firmware is reported via `downgraded` and left otherwise
/// untouched. That case happens after an OTA rollback, and the honest behaviour is to keep the
/// values — individual fields still load fine — rather than reset the user's configuration
/// because a version number looked unfamiliar.
MigrationResult migrateSettings(Settings& settings, uint32_t stored_schema);

}  // namespace dashboard::storage
