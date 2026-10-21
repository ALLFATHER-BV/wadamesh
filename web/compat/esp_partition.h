#pragma once
// Partitions: a host has no flash partition table. Lookups find nothing (but "tiles",
// below), so every partition-dependent path (OTA slots, coredump) takes its "not
// present" branch. Reads and writes are refused: nothing touches a partition raw.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_PARTITION_TYPE_APP = 0x00, ESP_PARTITION_TYPE_DATA = 0x01, ESP_PARTITION_TYPE_ANY = 0xff } esp_partition_type_t;
typedef enum {
  ESP_PARTITION_SUBTYPE_APP_FACTORY = 0x00, ESP_PARTITION_SUBTYPE_APP_OTA_MIN = 0x10,
  ESP_PARTITION_SUBTYPE_APP_OTA_0 = 0x10, ESP_PARTITION_SUBTYPE_APP_OTA_1 = 0x11,
  ESP_PARTITION_SUBTYPE_APP_OTA_MAX = 0x20, ESP_PARTITION_SUBTYPE_APP_TEST = 0x20,
  ESP_PARTITION_SUBTYPE_DATA_OTA = 0x00, ESP_PARTITION_SUBTYPE_DATA_PHY = 0x01,
  ESP_PARTITION_SUBTYPE_DATA_NVS = 0x02, ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 0x03,
  ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS = 0x04, ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM = 0x05,
  ESP_PARTITION_SUBTYPE_DATA_UNDEFINED = 0x06, ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD = 0x80,
  ESP_PARTITION_SUBTYPE_DATA_FAT = 0x81, ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 0x82,
  ESP_PARTITION_SUBTYPE_DATA_LITTLEFS = 0x83, ESP_PARTITION_SUBTYPE_ANY = 0xff
} esp_partition_subtype_t;
typedef struct esp_partition_t {
  void* flash_chip; esp_partition_type_t type; esp_partition_subtype_t subtype;
  uint32_t address; uint32_t size; uint32_t erase_size; char label[17]; bool encrypted; bool readonly;
} esp_partition_t;
typedef struct esp_partition_iterator_opaque_* esp_partition_iterator_t;
// One exception: the "tiles" data partition exists. The web board's LittleFS volume
// (fs_host.cpp, <data>/littlefs) IS the touch boards' tiles partition, and the UI mounts
// it only when the running table has one (UITask.cpp, the tile cache backend).
static inline const esp_partition_t* esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s, const char* l) {
  static const esp_partition_t k_tiles = { NULL, ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
                                           0x0, 0x4C0000, 0x1000, "tiles", false, false };
  if (l && t == ESP_PARTITION_TYPE_DATA && (s == ESP_PARTITION_SUBTYPE_ANY || s == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS) &&
      l[0] == 't' && l[1] == 'i' && l[2] == 'l' && l[3] == 'e' && l[4] == 's' && l[5] == 0)
    return &k_tiles;
  return NULL;
}
static inline esp_partition_iterator_t esp_partition_find(esp_partition_type_t t, esp_partition_subtype_t s, const char* l) { (void)t; (void)s; (void)l; return NULL; }
static inline const esp_partition_t* esp_partition_get(esp_partition_iterator_t it) { (void)it; return NULL; }
static inline esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it) { (void)it; return NULL; }
static inline void esp_partition_iterator_release(esp_partition_iterator_t it) { (void)it; }
static inline esp_err_t esp_partition_read(const esp_partition_t* p, size_t off, void* dst, size_t n) { (void)p; (void)off; (void)dst; (void)n; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_partition_write(const esp_partition_t* p, size_t off, const void* src, size_t n) { (void)p; (void)off; (void)src; (void)n; return ESP_ERR_NOT_SUPPORTED; }
static inline esp_err_t esp_partition_erase_range(const esp_partition_t* p, size_t off, size_t n) { (void)p; (void)off; (void)n; return ESP_ERR_NOT_SUPPORTED; }
#ifdef __cplusplus
}
#endif
