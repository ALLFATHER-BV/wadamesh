#pragma once
// heap_caps_* -> the C heap. Capability bits are accepted and ignored: the host has one
// heap, so "internal", "DMA" and "SPIRAM" requests all succeed from malloc. The size
// queries report a large fixed budget so the UI's low-memory guards stay quiet.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define MALLOC_CAP_EXEC     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT  (1 << 12)
#define MALLOC_CAP_IRAM_8BIT (1 << 13)
#define MALLOC_CAP_RETENTION (1 << 14)
#define MALLOC_CAP_RTCRAM   (1 << 15)
#define MALLOC_CAP_INVALID  (1 << 31)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct multi_heap_info_t {
  size_t total_free_bytes;
  size_t total_allocated_bytes;
  size_t largest_free_block;
  size_t minimum_free_bytes;
  size_t allocated_blocks;
  size_t free_blocks;
  size_t total_blocks;
} multi_heap_info_t;
#define WADA_HOST_HEAP_REPORT ((size_t)8 * 1024 * 1024)   // what the size queries claim is free
static inline void* heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return malloc(size); }
static inline void* heap_caps_calloc(size_t n, size_t size, uint32_t caps) { (void)caps; return calloc(n, size); }
static inline void* heap_caps_realloc(void* p, size_t size, uint32_t caps) { (void)caps; return realloc(p, size); }
static inline void* heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps) {
  (void)caps; void* p = NULL; if (align < sizeof(void*)) align = sizeof(void*);
  return posix_memalign(&p, align, size) == 0 ? p : NULL;
}
static inline void heap_caps_free(void* p) { free(p); }
static inline void* heap_caps_malloc_prefer(size_t size, size_t num, ...) { (void)num; return malloc(size); }
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return WADA_HOST_HEAP_REPORT; }
static inline size_t heap_caps_get_total_size(uint32_t caps) { (void)caps; return WADA_HOST_HEAP_REPORT * 2; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return WADA_HOST_HEAP_REPORT; }
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return WADA_HOST_HEAP_REPORT; }
static inline void heap_caps_get_info(multi_heap_info_t* info, uint32_t caps) {
  (void)caps;
  info->total_free_bytes = WADA_HOST_HEAP_REPORT; info->total_allocated_bytes = WADA_HOST_HEAP_REPORT;
  info->largest_free_block = WADA_HOST_HEAP_REPORT; info->minimum_free_bytes = WADA_HOST_HEAP_REPORT;
  info->allocated_blocks = 0; info->free_blocks = 1; info->total_blocks = 1;
}
static inline bool heap_caps_check_integrity_all(bool print) { (void)print; return true; }
static inline bool heap_caps_check_integrity(uint32_t caps, bool print) { (void)caps; (void)print; return true; }
static inline void heap_caps_print_heap_info(uint32_t caps) { (void)caps; }
static inline size_t esp_get_free_heap_size(void) { return WADA_HOST_HEAP_REPORT; }
static inline size_t esp_get_minimum_free_heap_size(void) { return WADA_HOST_HEAP_REPORT; }
#ifdef __cplusplus
}
#endif
