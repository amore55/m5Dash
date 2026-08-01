#include "dashboard/semver.hpp"

#include <cstdio>
#include <cstring>

namespace dashboard {
namespace {

/// Parse a run of digits into `out`. Returns the number of characters consumed, or 0 if the
/// first character was not a digit or the value would overflow.
size_t parseNumber(const char* s, uint32_t& out) {
    if (s == nullptr || *s < '0' || *s > '9') {
        return 0;
    }
    // Reject leading zeros ("01.2.3") — semver forbids them, and accepting them would make
    // two different strings compare equal, which is a nasty class of OTA bug.
    if (s[0] == '0' && s[1] >= '0' && s[1] <= '9') {
        return 0;
    }
    uint64_t value = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        value = value * 10 + static_cast<uint64_t>(s[i] - '0');
        if (value > 0xFFFFFFFFull) {
            return 0;
        }
        ++i;
    }
    out = static_cast<uint32_t>(value);
    return i;
}

}  // namespace

bool SemVer::parse(const char* text, SemVer& out) {
    if (text == nullptr) {
        return false;
    }
    const char* p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == 'v' || *p == 'V') {
        ++p;
    }

    SemVer parsed;

    size_t used = parseNumber(p, parsed.major);
    if (used == 0) {
        return false;
    }
    p += used;
    if (*p != '.') {
        return false;
    }
    ++p;

    used = parseNumber(p, parsed.minor);
    if (used == 0) {
        return false;
    }
    p += used;
    if (*p != '.') {
        return false;
    }
    ++p;

    used = parseNumber(p, parsed.patch);
    if (used == 0) {
        return false;
    }
    p += used;

    if (*p == '-') {
        ++p;
        const char* start = p;
        while (*p != '\0' && *p != '+' && *p != ' ') {
            ++p;
        }
        if (p == start) {
            return false;  // "1.2.3-" is malformed
        }
        parsed.prerelease.assign(start, static_cast<size_t>(p - start));
    }

    if (*p == '+') {
        // Build metadata: valid, ignored for ordering.
        ++p;
        if (*p == '\0') {
            return false;  // "1.2.3+" is malformed
        }
        while (*p != '\0' && *p != ' ') {
            ++p;
        }
    }

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    if (*p != '\0') {
        return false;  // trailing junk
    }

    out = parsed;
    return true;
}

int SemVer::compare(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    const bool a_pre = a.isPrerelease();
    const bool b_pre = b.isPrerelease();
    if (a_pre != b_pre) {
        return a_pre ? -1 : 1;  // a release outranks a pre-release
    }
    if (!a_pre) {
        return 0;
    }
    const int cmp = std::strcmp(a.prerelease.c_str(), b.prerelease.c_str());
    return (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
}

void SemVer::format(char* out, size_t out_len) const {
    if (out == nullptr || out_len == 0) {
        return;
    }
    if (isPrerelease()) {
        std::snprintf(out, out_len, "%u.%u.%u-%s", static_cast<unsigned>(major),
                      static_cast<unsigned>(minor), static_cast<unsigned>(patch),
                      prerelease.c_str());
    } else {
        std::snprintf(out, out_len, "%u.%u.%u", static_cast<unsigned>(major),
                      static_cast<unsigned>(minor), static_cast<unsigned>(patch));
    }
}

bool isUpgrade(const char* running, const char* candidate) {
    SemVer a;
    SemVer b;
    if (!SemVer::parse(running, a) || !SemVer::parse(candidate, b)) {
        return false;
    }
    return SemVer::compare(b, a) > 0;
}

bool satisfiesMinimum(const char* version, const char* minimum) {
    SemVer v;
    SemVer m;
    if (!SemVer::parse(version, v) || !SemVer::parse(minimum, m)) {
        return false;
    }
    return SemVer::compare(v, m) >= 0;
}

}  // namespace dashboard
