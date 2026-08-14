// A scratch buffer for one HTTP response, taken from PSRAM.
//
// WHY THIS EXISTS, AND WHY IT IS NOT JUST A MEMBER ARRAY
//
// The obvious way to give a plugin somewhere to put a response is a `char buf[N]` member on the
// plugin object. Plugins are statically allocated, so that array lands in .bss — which on the
// ESP32-P4 means INTERNAL SRAM, permanently, whether a fetch is in flight or not.
//
// Internal SRAM is the scarce resource on this board: roughly 500 KB in total, of which about
// 40 KB is still free at the low-water mark once the display, LVGL and the esp_hosted SDIO
// driver have taken their share. See docs/BACKLOG.md §1.3 — a TLS handshake with mbedtls
// allocating internally was enough to panic the device in esp_hosted's SDIO driver.
//
// TfL's arrivals response for Liverpool Street measures ~15 KB filtered, ~31 KB unfiltered. A
// buffer that size as a plugin member would consume more of the internal headroom than the TLS
// handshake it exists to serve. So the buffer comes from the 32 MB of PSRAM instead, exists only
// for the duration of a fetch, and the plugin holds nothing between refreshes.
//
// The consequence for callers: the buffer is a local in fetch(), and anything that must outlive
// the fetch — a cache write, a parsed model — has to be done before it goes out of scope. That is
// a feature. It is what stops a "last response" pointer outliving its storage.

#pragma once

#include <cstddef>

namespace dashboard::net {

class ResponseBuffer {
  public:
    /// Allocates `capacity` + 1 bytes (the extra is for the NUL that HttpsClient writes).
    /// Check valid() before use — an allocation failure is reported, never fatal.
    explicit ResponseBuffer(size_t capacity);
    ~ResponseBuffer();

    ResponseBuffer(const ResponseBuffer&) = delete;
    ResponseBuffer& operator=(const ResponseBuffer&) = delete;

    bool valid() const { return data_ != nullptr; }
    char* data() { return data_; }
    const char* data() const { return data_; }

    /// Total bytes available, including the terminator — i.e. what to pass to HttpsClient::get().
    size_t capacity() const { return capacity_; }

  private:
    char* data_ = nullptr;
    size_t capacity_ = 0;
};

}  // namespace dashboard::net
