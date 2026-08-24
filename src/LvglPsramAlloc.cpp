#include "LvglPsramAlloc.h"

#if defined(ESP32)
  #include <esp_heap_caps.h>
#endif
#include <stdlib.h>
#include <string.h>

// PSRAM allocations need MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT. The OR-ed mask
// means "must satisfy both" — i.e. backed by SPIRAM and byte-addressable
// (LVGL writes/reads bytes, never instruction fetches). Fall back to default
// malloc when SPIRAM is exhausted or the request is too small for the SPIRAM
// pool to satisfy efficiently (the IDF heap will sometimes refuse very small
// SPIRAM allocations and route through DRAM anyway).

extern "C" void* lvglPsramAlloc(size_t size) {
  if (size == 0) return nullptr;
#if defined(ESP32)
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return malloc(size);
}

extern "C" void lvglPsramFree(void* ptr) {
  if (!ptr) return;
#if defined(ESP32)
  // heap_caps_free works for both SPIRAM and DRAM allocations.
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

extern "C" void* lvglPsramRealloc(void* ptr, size_t size) {
  if (size == 0) {
    lvglPsramFree(ptr);
    return nullptr;
  }
  if (!ptr) {
    return lvglPsramAlloc(size);
  }
#if defined(ESP32)
  // ESP-IDF documents plain realloc(ptr, size) as
  // heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT). That generic capability can
  // move a PSRAM-backed LVGL block into the higher-priority internal heap. The
  // UI performs many small text/style reallocations while building its tree,
  // so the old implementation silently migrated roughly 60 KB back into the
  // DRAM needed by Wi-Fi/BLE connection and security work. Preserve the PSRAM
  // requirement explicitly. heap_caps_realloc handles copying even when ptr
  // came from the internal fallback in lvglPsramAlloc().
  void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  // PSRAM absent or exhausted: retain the original fallback semantics. A failed
  // heap_caps_realloc leaves ptr valid, so the ordinary realloc remains safe.
  return realloc(ptr, size);
}
