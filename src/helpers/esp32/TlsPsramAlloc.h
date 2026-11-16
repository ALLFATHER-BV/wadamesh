#pragma once

// mbedtls in this Arduino core is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: every
// TLS allocation (~32 KB of record buffers plus the handshake state) comes from
// internal DRAM, which a board running Wi-Fi, BLE and the web console does not have
// to spare (#870). The core keeps MBEDTLS_PLATFORM_MEMORY on, so the allocator can be
// swapped at runtime for a PSRAM-first one: the same placement as IDF's
// CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC, with an internal fallback. The hardware AES/SHA
// drivers bounce external-RAM buffers through internal DMA memory themselves.

#if defined(ESP32) && defined(BOARD_HAS_PSRAM) && __has_include(<mbedtls/platform.h>)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

inline bool& tlsPsramFlag() { static bool on = false; return on; }

inline void* tlsPsramCalloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
inline void tlsPsramFree(void* p) { heap_caps_free(p); }   // as esp_mbedtls_mem_free

// Call once at boot. Safe even after mbedtls has allocated: both allocators free with
// heap_caps_free, so a block from either can be released by the other.
inline bool tlsUsePsram() {
  if (!psramFound()) return false;
  if (mbedtls_platform_set_calloc_free(tlsPsramCalloc, tlsPsramFree) != 0) return false;
  tlsPsramFlag() = true;
  return true;
}
inline bool tlsAllocIsPsram() { return tlsPsramFlag(); }
#else
inline bool tlsUsePsram() { return false; }
inline bool tlsAllocIsPsram() { return false; }
#endif
