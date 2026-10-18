#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { size_t used_entries, free_entries, total_entries, namespace_count; } nvs_stats_t;
esp_err_t nvs_get_stats(const char* part_name, nvs_stats_t* stats);   // prefs_host.cpp
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_ERR_NVS_BASE 0x1100
#define ESP_ERR_NVS_NOT_FOUND (ESP_ERR_NVS_BASE + 0x02)
#define ESP_ERR_NVS_NO_FREE_PAGES (ESP_ERR_NVS_BASE + 0x0d)
#define ESP_ERR_NVS_NEW_VERSION_FOUND (ESP_ERR_NVS_BASE + 0x10)
// Only "does this namespace exist" (SdNvsPrefs asks before a quiet read-only open);
// reads and writes go through Preferences (prefs_host.cpp).
esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out);
void nvs_close(nvs_handle_t h);
#ifdef __cplusplus
}
#endif
