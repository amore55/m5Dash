// LVGL's allocator, pointed at PSRAM.
//
// WHY THIS FILE EXISTS — measured, not assumed.
//
// LVGL was built with CONFIG_LV_USE_CLIB_MALLOC, so every widget, style and label buffer went
// through plain malloc(). That reads as "PSRAM-backed, we have 32 MB", and it is wrong:
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL is 16384, meaning any allocation SMALLER than 16 KB is
// served from internal SRAM first and only falls back to PSRAM once internal is exhausted.
// Every LVGL allocation is far smaller than 16 KB, so the entire UI lived in the one pool that
// is actually scarce on this board.
//
// Measured at boot with heap_caps_get_free_size(MALLOC_CAP_INTERNAL):
//
//     PAGEMEM: internal before 149271 after 101727 => 47544 B for 6 pages
//
// ~7.9 KB of internal SRAM per page, resident forever — PageManager creates pages once and
// never destroys them. Against a steady-state internal free of ~65 KB and a low-water of
// ~26 KB during TLS handshakes, that made each new page a real cost and the summary page a
// question rather than a detail.
//
// So LVGL gets its own allocator that tries PSRAM FIRST and falls back to internal, which is
// exactly the inverse of the global policy. This is strictly better than raising
// SPIRAM_MALLOC_ALWAYSINTERNAL globally: that would push the Wi-Fi stack, the SDIO driver and
// every small driver buffer into PSRAM too, and some of those want to be internal for latency
// or DMA reasons. Here the change is scoped to the UI, which is the one large consumer that
// does not care.
//
// The fallback matters and is not decoration: if PSRAM is ever exhausted or unavailable, LVGL
// keeps working from internal RAM rather than returning NULL and taking the display down.
//
// LVGL requires the full lv_*_core set when LV_USE_STDLIB_MALLOC is LV_STDLIB_CUSTOM. This
// mirrors lvgl/src/stdlib/clib/lv_mem_core_clib.c, including the deliberately unsupported
// pool and monitor entry points.

#include <stddef.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

/// PSRAM, byte-addressable. Deliberately NOT MALLOC_CAP_DMA: LVGL's draw buffers are allocated
/// by esp_lvgl_port with their own caps, and nothing allocated through here is handed to DMA.
#define LVGL_HEAP_PREFERRED (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {
    /* Nothing to init — the IDF heap is already up. */
}

void lv_mem_deinit(void) {
    /* Nothing to deinit. */
}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    /* Not supported: this allocator has no arena of its own to extend. */
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    /* Not supported — see lv_mem_add_pool(). */
    LV_UNUSED(pool);
}

void* lv_malloc_core(size_t size) {
    void* p = heap_caps_malloc(size, LVGL_HEAP_PREFERRED);
    if (p == NULL) {
        p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    }
    return p;
}

void* lv_realloc_core(void* p, size_t new_size) {
    // A failed heap_caps_realloc leaves the original block valid, so retrying on the default
    // heap is safe and cannot leak the old pointer.
    void* q = heap_caps_realloc(p, new_size, LVGL_HEAP_PREFERRED);
    if (q == NULL) {
        q = heap_caps_realloc(p, new_size, MALLOC_CAP_DEFAULT);
    }
    return q;
}

void lv_free_core(void* p) {
    // Correct for both capability sets: the IDF heap knows which pool a block came from.
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    /* Not supported: the IDF heap has no per-consumer accounting. Use the app's own
       health line (heap_caps_get_free_size) instead. */
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void) {
    /* Not supported — integrity is the IDF heap's business, via CONFIG_HEAP_POISONING. */
    return LV_RESULT_OK;
}
