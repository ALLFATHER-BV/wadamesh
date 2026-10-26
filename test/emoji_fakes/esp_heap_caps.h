// Host stand-in for ESP-IDF's heap_caps_* with a switch to make the next
// allocations fail, so the test can drive the out-of-memory path.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_8BIT (1 << 2)
static int g_fake_alloc_fail = 0;   // >0: fail every allocation while set
static int g_fake_allocs = 0;
static inline void* heap_caps_malloc(size_t n, unsigned) {
  if (g_fake_alloc_fail) return nullptr;
  ++g_fake_allocs;
  return malloc(n);
}
static inline void* heap_caps_calloc(size_t c, size_t n, unsigned) {
  if (g_fake_alloc_fail) return nullptr;
  ++g_fake_allocs;
  return calloc(c, n);
}
static inline void heap_caps_free(void* p) { free(p); }
