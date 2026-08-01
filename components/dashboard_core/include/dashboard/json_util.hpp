// Defensive cJSON accessors.
//
// Every upstream response in this project is parsed through these. The rule they enforce is
// simple and is the whole point: **a missing field, a null, or a field of the wrong type is a
// normal outcome, not an error to crash on.** Each accessor returns false and leaves the
// output untouched, so a caller can decide field-by-field what is optional.
//
// This exists because the alternative — `item->valuestring` after a bare
// cJSON_GetObjectItem — dereferences NULL the first time an API omits a field or returns
// `null`, and public weather/transport APIs do that routinely.
//
// No ESP-IDF dependency; the host tests parse the same fixtures through this code.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dashboard/fixed_string.hpp"
#include "dashboard/json_compat.hpp"

namespace dashboard::json {

/// RAII wrapper around a parsed document. Non-copyable, movable.
///
/// Always parses with an explicit length (cJSON_ParseWithLength) rather than relying on a NUL
/// terminator, because an HTTP body is a buffer of known size and may legitimately contain no
/// terminator at all.
class Doc {
  public:
    Doc() = default;
    ~Doc();

    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    Doc(Doc&& other) noexcept;
    Doc& operator=(Doc&& other) noexcept;

    /// Replaces any previously held document. Returns false on a parse failure or empty input.
    bool parse(const char* data, size_t len);

    void reset();

    const cJSON* root() const { return root_; }
    bool valid() const { return root_ != nullptr; }
    explicit operator bool() const { return valid(); }

  private:
    cJSON* root_ = nullptr;
};

// ---------------------------------------------------------------------------------------
// Field accessors. `parent` may be null — that simply yields "not found".
// ---------------------------------------------------------------------------------------

/// Child object by key. Returns null unless the child exists AND is an object.
const cJSON* object(const cJSON* parent, const char* key);

/// Child array by key. Returns null unless the child exists AND is an array.
const cJSON* array(const cJSON* parent, const char* key);

/// Any child by key, whatever its type. Returns null if absent.
const cJSON* item(const cJSON* parent, const char* key);

/// Copies a string field into a fixed buffer, always NUL-terminating.
/// Returns false if absent, null, or not a string. Truncates silently if too long.
bool string(const cJSON* parent, const char* key, char* out, size_t out_len);

template <size_t N>
bool string(const cJSON* parent, const char* key, FixedString<N>& out) {
    char buf[N];
    if (!string(parent, key, buf, sizeof(buf))) {
        return false;
    }
    out.assign(buf);
    return true;
}

bool number(const cJSON* parent, const char* key, double& out);
bool integer(const cJSON* parent, const char* key, int32_t& out);
bool integer64(const cJSON* parent, const char* key, int64_t& out);
bool boolean(const cJSON* parent, const char* key, bool& out);

/// As number(), but also accepts a numeric value delivered as a JSON string ("12.5").
/// Some APIs are inconsistent about this between fields; using this for values that are
/// semantically numeric avoids a spurious parse failure.
bool numberLoose(const cJSON* parent, const char* key, double& out);

/// True if the key is present and is JSON null. Distinguishes "explicitly no value" from
/// "field absent", which matters for e.g. an hourly forecast with gaps.
bool isNull(const cJSON* parent, const char* key);

// ---------------------------------------------------------------------------------------
// Array access
// ---------------------------------------------------------------------------------------

/// 0 for a null or non-array argument.
size_t arraySize(const cJSON* array_node);

/// Null if out of range or not an array. Bounds-checked, unlike cJSON_GetArrayItem.
const cJSON* at(const cJSON* array_node, size_t index);

/// Read element `index` of an array of numbers. False if absent or not numeric.
bool numberAt(const cJSON* array_node, size_t index, double& out);

/// Read element `index` of an array of strings into a fixed buffer.
bool stringAt(const cJSON* array_node, size_t index, char* out, size_t out_len);

/// Value of a top-level string on a node that IS the string (not a child lookup).
bool valueString(const cJSON* node, char* out, size_t out_len);

}  // namespace dashboard::json
