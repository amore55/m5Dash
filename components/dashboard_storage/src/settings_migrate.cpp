#include "dashboard/storage/settings_migrate.hpp"

namespace dashboard::storage {
namespace {

/// Schema 0 -> 1.
///
/// Schema 0 means "no schema key was stored", which is either a brand-new device or one
/// provisioned before versioning existed. Nothing needs rewriting: every field either loaded
/// from its own key or took its default. The step exists so the version is stamped and the
/// migration chain has a starting link.
void migrate0to1(Settings& settings) { settings.schema = 1; }

// ---------------------------------------------------------------------------------------
// ADDING A MIGRATION
//
//   1. Bump Settings::kCurrentSchema.
//   2. Add a migrateNtoN+1(Settings&) here that transforms ONLY what changed meaning.
//   3. Add it to the chain in migrateSettings().
//   4. Add a case to test/host/src/test_settings_migrate.cpp proving an old value lands
//      correctly — these run on a device holding real user configuration, so they are exactly
//      the kind of code that deserves a test.
//
// Migrations must be idempotent and must not depend on anything outside `settings`.
// ---------------------------------------------------------------------------------------

}  // namespace

MigrationResult migrateSettings(Settings& settings, uint32_t stored_schema) {
    MigrationResult result;
    result.from_schema = stored_schema;
    result.to_schema = Settings::kCurrentSchema;

    if (stored_schema > Settings::kCurrentSchema) {
        // Written by newer firmware — almost certainly an OTA rollback. Keep the values: the
        // individual fields still load correctly, and discarding someone's configuration
        // because a version number is unfamiliar would be a far worse outcome than carrying
        // forward a setting this build happens not to use.
        result.downgraded = true;
        settings.schema = stored_schema;
        return result;
    }

    if (stored_schema == Settings::kCurrentSchema) {
        return result;
    }

    uint32_t schema = stored_schema;
    if (schema < 1) {
        migrate0to1(settings);
        schema = 1;
    }
    // Future steps chain here: if (schema < 2) { migrate1to2(settings); schema = 2; }

    settings.schema = Settings::kCurrentSchema;
    result.changed = true;
    return result;
}

}  // namespace dashboard::storage
