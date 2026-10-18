#pragma once
// No panic coredump on a host: the crash-report export finds nothing.
#include <stddef.h>
#include "esp_err.h"
static inline esp_err_t esp_core_dump_image_check(void) { return ESP_ERR_NOT_FOUND; }
static inline esp_err_t esp_core_dump_image_get(size_t* addr, size_t* size) { (void)addr; (void)size; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t esp_core_dump_image_erase(void) { return ESP_ERR_NOT_FOUND; }
