// The OTA manifest: a small JSON document describing the newest firmware on a channel.
//
// ESP-free and LVGL-free, like every other model in this project — parsing and the
// upgrade-worthiness decision are both plain data logic, testable against a saved fixture with
// no device involved.
//
// THE SCHEMA, since nothing in the repo defined one concretely before this:
//
//     {
//       "channel": "stable",
//       "version": "0.2.0",
//       "minimum_version": "0.1.0",
//       "url": "https://github.com/OWNER/REPO/releases/latest/download/app.bin",
//       "sha256": "64 lowercase hex characters",
//       "size": 1234567,
//       "notes": "optional, human text shown on the settings page"
//     }
//
// `channel` is the manifest's OWN claim about what it is, checked against the device's
// configured channel rather than trusted from the URL alone — the same "don't trust the
// transport, check the payload" instinct as parsing a departure board rather than assuming the
// query parameter was honoured.
//
// `minimum_version` is OPTIONAL and, when present, is a floor: a device running something older
// than it must not attempt this update directly (a schema or partition-layout change that only
// makes sense stepping through an intermediate release). Absent means "no floor beyond newer".
//
// `sha256` and `size` are both mandatory and both enforced BEFORE a single byte reaches the OTA
// partition — see ota_service.hpp for why size is checked first, cheaply, while sha256 can only
// be checked once the whole image has streamed past.

#pragma once

#include <cstddef>

#include "dashboard/fixed_string.hpp"

namespace dashboard::ota {

struct Manifest {
    bool valid = false;

    ShortString channel;
    ShortString version;

    /// Empty when the manifest declares no floor.
    ShortString minimum_version;

    UrlString url;

    /// 64 lowercase hex characters, compared case-insensitively against the digest computed
    /// while streaming. Fixed-length rather than arbitrary-length: a value of any other length
    /// is not a SHA-256 digest and parsing rejects it outright rather than storing a partial one.
    FixedString<65> sha256;

    size_t size = 0;

    /// Optional, shown verbatim on the settings page. Never used in any decision.
    MediumString notes;
};

/// Parse a manifest response. Rejects (returns false) anything missing `version`, `url`, `sha256`
/// of the wrong length, or `size`. `channel` and `minimum_version` are read if present, defaulted
/// to empty otherwise — an empty channel is caught by the CALLER's channel check (an empty
/// manifest channel can never equal a device's non-empty configured channel), and an empty
/// minimum_version already means "no floor" by design.
bool parseManifest(const char* json, size_t len, Manifest& out);

}  // namespace dashboard::ota
