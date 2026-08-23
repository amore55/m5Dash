#include "plugins/quote_provider.hpp"

#include <cstring>

#include "esp_log.h"

#include "dashboard/json_util.hpp"
#include "dashboard/storage/secret_store.hpp"
#include "dashboard/time_utils.hpp"

namespace plugins {
namespace {

namespace json = dashboard::json;
namespace timeutil = dashboard::timeutil;
using dashboard::storage::Secret;
using dashboard::storage::SecretStore;

constexpr const char* kTag = "quote";
constexpr const char* kUrl = "https://api.api-ninjas.com/v2/quotes";

/// api-ninjas names this header exactly. It is a credential, so HttpsClient will not log its
/// value and will not follow a redirect while carrying it.
constexpr const char* kKeyHeader = "X-Api-Key";

/// Pull the fields off whichever node actually holds them.
bool readQuoteObject(const cJSON* node, Quote& out) {
    if (node == nullptr) {
        return false;
    }
    if (!json::string(node, "quote", out.text)) {
        return false;
    }
    // An attributed quote is the norm but not guaranteed; an anonymous one is still a quote.
    if (!json::string(node, "author", out.author)) {
        out.author.clear();
    }
    return !out.text.empty();
}

}  // namespace

int64_t quoteBucket(std::time_t now_utc, bool time_valid) {
    if (!time_valid || now_utc <= 0) {
        return -1;
    }
    std::tm local = {};
    if (localtime_r(&now_utc, &local) == nullptr) {
        return -1;
    }
    // Two buckets per local day. Derived from the calendar rather than from now_utc/43200 so the
    // changeover happens at local noon, which is what "twice a day" means to a reader.
    const int64_t day = timeutil::daysFromCivil(local.tm_year + 1900, local.tm_mon + 1,
                                                local.tm_mday);
    return day * 2 + (local.tm_hour >= 12 ? 1 : 0);
}

bool parseQuote(const char* json_text, size_t len, Quote& out) {
    out = Quote{};

    json::Doc doc;
    if (!doc.parse(json_text, len)) {
        return false;
    }

    // Both shapes: a one-element array (what v1 returned) or a bare object.
    const cJSON* root = doc.root();
    if (cJSON_IsArray(root)) {
        if (!readQuoteObject(json::at(root, 0), out)) {
            return false;
        }
    } else if (!readQuoteObject(root, out)) {
        return false;
    }

    out.valid = true;
    return true;
}

bool QuoteProvider::configured() { return SecretStore::has(Secret::QuoteApiKey); }

esp_err_t QuoteProvider::fetch(char* buffer, size_t capacity, Quote& out) {
    if (!configured()) {
        last_error_ = "no api-ninjas key";
        return ESP_ERR_INVALID_STATE;
    }

    char key[dashboard::storage::kMaxSecretLength + 1] = {};
    if (SecretStore::get(Secret::QuoteApiKey, key, sizeof(key)) != ESP_OK) {
        last_error_ = "could not read the key";
        return ESP_FAIL;
    }

    dashboard::net::HttpRequest request;
    request.url = kUrl;
    request.header_name = kKeyHeader;
    request.header_value = key;
    // One attempt. A quote is decoration on a page whose real job is telling the time, and it is
    // asked for twice a day — retrying a failure costs the request budget for no visible gain,
    // and the previous quote stays on screen either way.
    request.max_attempts = 1;

    dashboard::net::HttpResponse response;
    const esp_err_t err = http_.get(request, buffer, capacity, response);

    // Wipe the key from the stack before anything else can happen with this frame.
    std::memset(key, 0, sizeof(key));

    if (err != ESP_OK) {
        // The BODY is safe to log — the credential was in a header. Worth having: this endpoint
        // reports configuration problems (a bad or expired key) in the body with a 4xx, and
        // without it that is indistinguishable from the network being down.
        if (response.length > 0) {
            ESP_LOGW(kTag, "HTTP %d: %.120s", response.status, buffer);
        }
        last_error_ = response.status == 401 || response.status == 400 ? "api-ninjas rejected the key"
                                                                      : "quote fetch failed";
        return err;
    }

    if (!parseQuote(buffer, response.length, out)) {
        ESP_LOGW(kTag, "unexpected response shape: %.120s", buffer);
        last_error_ = "quote response not understood";
        return ESP_ERR_INVALID_RESPONSE;
    }

    last_error_ = "";
    return ESP_OK;
}

}  // namespace plugins
