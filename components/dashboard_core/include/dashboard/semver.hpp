// Semantic version parsing and comparison.
//
// Used by the OTA service to decide whether a manifest offers an upgrade, and to enforce
// the manifest's minimum_version. Deliberately strict: a version it cannot parse is never
// treated as newer than the running firmware, so a malformed manifest can never trigger an
// install. That is why the local development version is "0.0.0-dev" — parseable, but the
// lowest possible precedence.
//
// Scope: MAJOR.MINOR.PATCH plus an optional pre-release identifier. Build metadata after
// '+' is parsed and ignored for ordering, per semver.org. Pre-release ordering is the
// simplified rule below, which is enough for "stable vs -dev/-rc" channels.
//
// No ESP-IDF dependency — covered by test/host/src/test_semver.cpp.

#pragma once

#include <cstdint>

#include "dashboard/fixed_string.hpp"

namespace dashboard {

struct SemVer {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    FixedString<24> prerelease;  // "" for a release build, e.g. "rc.1" or "dev"

    /// Parse "1.2.3", "v1.2.3", "1.2.3-rc.1", "1.2.3+abc123", "v1.2.3-dev+deadbeef".
    /// Returns false (and leaves `out` untouched) for anything else, including a missing
    /// patch component: "1.2" is rejected rather than assumed to mean "1.2.0".
    static bool parse(const char* text, SemVer& out);

    /// -1 if a < b, 0 if equal precedence, +1 if a > b.
    /// A release outranks any pre-release of the same MAJOR.MINOR.PATCH.
    /// Two pre-releases are ordered lexicographically, which is not the full semver
    /// dot-separated-identifier rule but is well-defined and sufficient here.
    static int compare(const SemVer& a, const SemVer& b);

    bool isPrerelease() const { return !prerelease.empty(); }

    /// Render back to "MAJOR.MINOR.PATCH[-PRERELEASE]".
    void format(char* out, size_t out_len) const;

    bool operator<(const SemVer& o) const { return compare(*this, o) < 0; }
    bool operator>(const SemVer& o) const { return compare(*this, o) > 0; }
    bool operator==(const SemVer& o) const { return compare(*this, o) == 0; }
};

/// Convenience for the OTA path: is `candidate` a strict upgrade over `running`?
/// Returns false if either string fails to parse.
bool isUpgrade(const char* running, const char* candidate);

/// Is `version` >= `minimum`? Returns false if either fails to parse, so a manifest with a
/// broken minimum_version blocks the install rather than permitting it.
bool satisfiesMinimum(const char* version, const char* minimum);

}  // namespace dashboard
