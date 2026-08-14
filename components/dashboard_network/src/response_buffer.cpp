#include "dashboard/net/response_buffer.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace dashboard::net {
namespace {
constexpr const char* kTag = "httpbuf";
}

ResponseBuffer::ResponseBuffer(size_t capacity) {
    if (capacity == 0) {
        return;
    }
    const size_t bytes = capacity + 1;  // room for the terminator HttpsClient writes

    // PSRAM first, explicitly. Not plain malloc(): with CONFIG_SPIRAM_USE_MALLOC the allocator may
    // satisfy a request from internal SRAM, which is the one thing this class exists to avoid.
    data_ = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));

    if (data_ == nullptr) {
        // Fall back rather than fail the fetch. A board with no PSRAM, or PSRAM exhausted by
        // something else, should still be able to read a small response.
        ESP_LOGW(kTag, "no PSRAM for %u bytes; falling back to internal",
                 static_cast<unsigned>(bytes));
        data_ = static_cast<char*>(heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT));
    }

    if (data_ != nullptr) {
        capacity_ = bytes;
        data_[0] = '\0';
    } else {
        ESP_LOGE(kTag, "could not allocate %u bytes for a response",
                 static_cast<unsigned>(bytes));
    }
}

ResponseBuffer::~ResponseBuffer() {
    if (data_ != nullptr) {
        heap_caps_free(data_);
    }
}

}  // namespace dashboard::net
