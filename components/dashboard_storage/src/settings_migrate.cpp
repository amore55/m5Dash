#include "dashboard/storage/settings_migrate.hpp"

#include <cstdio>
#include <cstring>

namespace dashboard::storage {
namespace {

/// Schema 0 -> 1.
///
/// Schema 0 means "no schema key was stored", which is either a brand-new device or one
/// provisioned before versioning existed. Nothing needs rewriting: every field either loaded
/// from its own key or took its default. The step exists so the version is stamped and the
/// migration chain has a starting link.
void migrate0to1(Settings& settings) { settings.schema = 1; }

/// Schema 1 -> 2. The summary page arrived and became the hub.
///
/// Two things have to be rewritten, and only where the user has not chosen otherwise:
///
///   * `default_page` — a device provisioned before the summary page existed holds "clock",
///     which is not a preference so much as the old default. Rewritten ONLY when it still says
///     "clock"; anything else is a deliberate choice and is left alone.
///   * `page_order` — a stored order from schema 1 cannot mention "summary" or "github", and
///     PageManager appends unlisted ids in registration order. That would leave the hub at the
///     far end of the rotation from where it now belongs, so the two new ids are prepended and
///     appended respectively rather than the whole order being discarded.
void migrate1to2(Settings& settings) {
    if (settings.default_page.equals("clock")) {
        settings.default_page.assign("summary");
    }

    if (!settings.page_order.empty()) {
        // Deliberately larger than the field: the prefix and suffix cannot both fit alongside a
        // maximal stored value, and assign() then bounds the result. Sizing this buffer AT the
        // field's capacity is what -Werror=format-truncation objects to, and rightly — it could
        // drop ",github" off the end.
        char rebuilt[decltype(settings.page_order)::capacity() + 32];
        const bool has_summary = std::strstr(settings.page_order.c_str(), "summary") != nullptr;
        const bool has_github = std::strstr(settings.page_order.c_str(), "github") != nullptr;
        std::snprintf(rebuilt, sizeof(rebuilt), "%s%s%s", has_summary ? "" : "summary,",
                      settings.page_order.c_str(), has_github ? "" : ",github");
        settings.page_order.assign(rebuilt);
    }

    settings.schema = 2;
}

/// Schema 2 -> 3. The to-do placeholder became the calendar page.
///
/// The old id "todos" is renamed to "calendar" wherever it appears, TOKEN by token — a substring
/// replace would also mangle a hypothetical future id containing "todos" as a fragment. Neither
/// list is discarded, because a stored ORDER or a stored disabled-flag is a deliberate choice
/// about a page slot, and that slot still exists — it just shows something different now.
///
/// `enabled_pages` is included as well as `page_order`: a device where the owner had disabled the
/// to-do placeholder must not have the calendar page silently re-enabled just because the id
/// changed underneath it.
void renameTokenInCsv(FixedString<224>& field, const char* from, const char* to) {
    if (field.empty() || std::strstr(field.c_str(), from) == nullptr) {
        return;
    }
    // 224 matches FixedString<224>::capacity() above — not decltype(field)::capacity(), because
    // `field` is a reference and a reference type has no static members of its own to name.
    char rebuilt[224 + 1] = {};
    size_t out_len = 0;
    const char* cursor = field.c_str();
    while (*cursor != '\0' && out_len + 1 < sizeof(rebuilt)) {
        const char* comma = std::strchr(cursor, ',');
        const size_t token_len =
            (comma != nullptr) ? static_cast<size_t>(comma - cursor) : std::strlen(cursor);
        const bool matches = token_len == std::strlen(from) && std::strncmp(cursor, from, token_len) == 0;
        const char* token = matches ? to : cursor;
        const size_t token_out_len = matches ? std::strlen(to) : token_len;

        if (out_len != 0) {
            rebuilt[out_len++] = ',';
        }
        const size_t copy = (token_out_len < sizeof(rebuilt) - out_len - 1)
                               ? token_out_len
                               : sizeof(rebuilt) - out_len - 1;
        std::memcpy(rebuilt + out_len, token, copy);
        out_len += copy;

        cursor = (comma != nullptr) ? comma + 1 : cursor + token_len;
    }
    rebuilt[out_len] = '\0';
    field.assign(rebuilt);
}

void migrate2to3(Settings& settings) {
    renameTokenInCsv(settings.page_order, "todos", "calendar");

    // enabled_pages is the shorter FixedString<160>, not <224> — renameTokenInCsv only takes the
    // page_order type, so build a throwaway <224> copy, rename in it, then assign back. Cheap and
    // avoids a second near-identical function for one field.
    FixedString<224> enabled_copy(settings.enabled_pages.c_str());
    renameTokenInCsv(enabled_copy, "todos", "calendar");
    settings.enabled_pages.assign(enabled_copy.c_str());

    if (settings.default_page.equals("todos")) {
        settings.default_page.assign("calendar");
    }

    settings.schema = 3;
}

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
    if (schema < 2) {
        migrate1to2(settings);
        schema = 2;
    }
    if (schema < 3) {
        migrate2to3(settings);
        schema = 3;
    }
    // Future steps chain here: if (schema < 4) { migrate3to4(settings); schema = 4; }

    settings.schema = Settings::kCurrentSchema;
    result.changed = true;
    return result;
}

}  // namespace dashboard::storage
