#include "dashboard/json_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dashboard::json {

Doc::~Doc() { reset(); }

Doc::Doc(Doc&& other) noexcept : root_(other.root_) { other.root_ = nullptr; }

Doc& Doc::operator=(Doc&& other) noexcept {
    if (this != &other) {
        reset();
        root_ = other.root_;
        other.root_ = nullptr;
    }
    return *this;
}

void Doc::reset() {
    if (root_ != nullptr) {
        cJSON_Delete(root_);
        root_ = nullptr;
    }
}

bool Doc::parse(const char* data, size_t len) {
    reset();
    if (data == nullptr || len == 0) {
        return false;
    }
    // Length-explicit parse: an HTTP body is a sized buffer and is not guaranteed to be
    // NUL-terminated. cJSON_Parse() would read past the end.
    root_ = cJSON_ParseWithLength(data, len);
    return root_ != nullptr;
}

// ---------------------------------------------------------------------------------------

const cJSON* item(const cJSON* parent, const char* key) {
    if (parent == nullptr || key == nullptr) {
        return nullptr;
    }
    return cJSON_GetObjectItemCaseSensitive(parent, key);
}

const cJSON* object(const cJSON* parent, const char* key) {
    const cJSON* node = item(parent, key);
    return (node != nullptr && cJSON_IsObject(node)) ? node : nullptr;
}

const cJSON* array(const cJSON* parent, const char* key) {
    const cJSON* node = item(parent, key);
    return (node != nullptr && cJSON_IsArray(node)) ? node : nullptr;
}

bool string(const cJSON* parent, const char* key, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return false;
    }
    const cJSON* node = item(parent, key);
    if (node == nullptr || !cJSON_IsString(node) || node->valuestring == nullptr) {
        return false;
    }
    std::snprintf(out, out_len, "%s", node->valuestring);
    return true;
}

bool valueString(const cJSON* node, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return false;
    }
    if (node == nullptr || !cJSON_IsString(node) || node->valuestring == nullptr) {
        return false;
    }
    std::snprintf(out, out_len, "%s", node->valuestring);
    return true;
}

bool number(const cJSON* parent, const char* key, double& out) {
    const cJSON* node = item(parent, key);
    if (node == nullptr || !cJSON_IsNumber(node)) {
        return false;
    }
    out = node->valuedouble;
    return true;
}

bool numberLoose(const cJSON* parent, const char* key, double& out) {
    const cJSON* node = item(parent, key);
    if (node == nullptr) {
        return false;
    }
    if (cJSON_IsNumber(node)) {
        out = node->valuedouble;
        return true;
    }
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        char* end = nullptr;
        const double parsed = std::strtod(node->valuestring, &end);
        // Require that at least one character was consumed and that the remainder is only
        // whitespace, so "12abc" is rejected rather than read as 12.
        if (end == node->valuestring) {
            return false;
        }
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
            ++end;
        }
        if (*end != '\0') {
            return false;
        }
        out = parsed;
        return true;
    }
    return false;
}

bool integer(const cJSON* parent, const char* key, int32_t& out) {
    double value = 0.0;
    if (!number(parent, key, value)) {
        return false;
    }
    if (value < static_cast<double>(INT32_MIN) || value > static_cast<double>(INT32_MAX)) {
        return false;
    }
    out = static_cast<int32_t>(value);
    return true;
}

bool integer64(const cJSON* parent, const char* key, int64_t& out) {
    const cJSON* node = item(parent, key);
    if (node == nullptr || !cJSON_IsNumber(node)) {
        return false;
    }
    // cJSON stores every number as a double. Telegram update_ids and Unix timestamps are the
    // values that matter here, and both sit comfortably inside a double's 53-bit exact integer
    // range, so this is lossless in practice — but reject anything beyond it rather than
    // silently rounding.
    const double value = node->valuedouble;
    constexpr double kMaxExact = 9007199254740992.0;  // 2^53
    if (value < -kMaxExact || value > kMaxExact) {
        return false;
    }
    out = static_cast<int64_t>(value);
    return true;
}

bool boolean(const cJSON* parent, const char* key, bool& out) {
    const cJSON* node = item(parent, key);
    if (node == nullptr || !cJSON_IsBool(node)) {
        return false;
    }
    out = cJSON_IsTrue(node);
    return true;
}

bool isNull(const cJSON* parent, const char* key) {
    const cJSON* node = item(parent, key);
    return node != nullptr && cJSON_IsNull(node);
}

// ---------------------------------------------------------------------------------------

size_t arraySize(const cJSON* array_node) {
    if (array_node == nullptr || !cJSON_IsArray(array_node)) {
        return 0;
    }
    const int size = cJSON_GetArraySize(array_node);
    return size > 0 ? static_cast<size_t>(size) : 0;
}

const cJSON* at(const cJSON* array_node, size_t index) {
    if (index >= arraySize(array_node)) {
        return nullptr;
    }
    return cJSON_GetArrayItem(array_node, static_cast<int>(index));
}

bool numberAt(const cJSON* array_node, size_t index, double& out) {
    const cJSON* node = at(array_node, index);
    if (node == nullptr || !cJSON_IsNumber(node)) {
        return false;
    }
    out = node->valuedouble;
    return true;
}

bool stringAt(const cJSON* array_node, size_t index, char* out, size_t out_len) {
    return valueString(at(array_node, index), out, out_len);
}

}  // namespace dashboard::json
