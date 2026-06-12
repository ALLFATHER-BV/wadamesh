#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL's general allocator routed to prefer PSRAM. Internal DRAM is precious
// on this board (320 KB total, WiFi DMA wants ~10–15 KB for association), so
// the ~60 KB of widget state LVGL grows during buildUiTree goes to the 8 MB
// of PSRAM instead. Falls back to default malloc if PSRAM allocation fails
// (e.g. tiny allocations the heap may refuse to satisfy from SPIRAM).
void* lvglPsramAlloc(size_t size);
void  lvglPsramFree(void* ptr);
void* lvglPsramRealloc(void* ptr, size_t size);

#ifdef __cplusplus
}
#endif
