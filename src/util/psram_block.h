/**
 * One long-lived block, PSRAM first.
 *
 * ==[ WHY THIS EXISTS AS A HEADER ]== three modules had written the same six
 * lines: try SPIRAM, fall back to internal DRAM, zero it. They are the modules
 * whose state is too big for Core2's dram0_0_seg — the C5 bridge's scan
 * results, the mesh bridge's message ring, the mesh screen's wrap index — and
 * they all want the same policy for the same reason, so the policy should have
 * one home. Anything added to it (an OOM counter, a DMA cap, a failure log)
 * would otherwise have to be added three times.
 *
 * ==[ THE FALLBACK IS DELIBERATE ]== an explicit MALLOC_CAP_INTERNAL rather
 * than a bare malloc(). On a board with no PSRAM fitted, or with the SPIRAM
 * allocator exhausted, the caller would rather have DRAM than nothing — these
 * are all allocate-once-at-begin() blocks, so the fragmentation cost is paid a
 * single time and never again.
 *
 * ==[ NOT FOR HOT PATHS ]== callers allocate once at begin() and hold the
 * pointer for the life of the session. PSRAM is roughly an order of magnitude
 * slower than DRAM per access, which is fine for a message ring touched a few
 * times a minute and wrong for anything per-frame.
 */
#pragma once

#include <esp_heap_caps.h>
#include <stddef.h>
#include <string.h>

namespace PsramBlock {

// Zeroed on success, nullptr when neither heap could serve it. Callers are
// expected to degrade rather than crash: every user of this is a feature that
// can honestly report itself unavailable.
inline void* alloc(size_t bytes) {
    void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (memory) memset(memory, 0, bytes);
    return memory;
}

template <typename T>
inline T* allocFor() {
    return static_cast<T*>(alloc(sizeof(T)));
}

}  // namespace PsramBlock
