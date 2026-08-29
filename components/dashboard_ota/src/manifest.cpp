#include "dashboard/ota/manifest.hpp"

#include <cctype>
#include <cstring>

#include "dashboard/json_util.hpp"

namespace dashboard::ota {
namespace {

namespace json = dashboard::json;

/// True if `text` is exactly 64 hexadecimal characters. Anything else cannot be a SHA-256 digest,
/// and accepting it anyway would mean comparing the downloaded image's real digest against a
/// value that was never going to match — a manifest bug turning into a permanently-failing OTA
/// rather than being caught at parse time, where it is obvious what went wrong.
bool looksLikeSha256(const char* text) {
    if (text == nullptr || std::strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (std::isxdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool parseManifest(const char* json_text, size_t len, Manifest& out) {
    out = Manifest{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }
    const cJSON* root = doc.root();

    if (!json::string(root, "version", out.version) || out.version.empty()) {
        return false;
    }
    if (!json::string(root, "url", out.url) || out.url.empty()) {
        return false;
    }

    char sha256[65] = {};
    if (!json::string(root, "sha256", sha256, sizeof(sha256)) || !looksLikeSha256(sha256)) {
        return false;
    }
    // Stored lower-case so the caller's comparison against a computed digest (which mbedtls
    // renders lower-case) can be a plain strcmp rather than a case-insensitive one.
    for (size_t i = 0; i < 64; ++i) {
        sha256[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(sha256[i])));
    }
    out.sha256.assign(sha256);

    double size_value = 0.0;
    if (!json::number(root, "size", size_value) || size_value <= 0.0) {
        return false;
    }
    out.size = static_cast<size_t>(size_value);

    // Optional fields. Absence is not a parse failure — see the header.
    json::string(root, "channel", out.channel);
    json::string(root, "minimum_version", out.minimum_version);
    json::string(root, "notes", out.notes);

    out.valid = true;
    return true;
}

}  // namespace dashboard::ota
