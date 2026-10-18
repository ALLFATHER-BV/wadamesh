#pragma once
#include <stdint.h>
typedef enum { CHIP_ESP32 = 1, CHIP_ESP32S2 = 2, CHIP_ESP32S3 = 9, CHIP_ESP32C3 = 5, CHIP_ESP32P4 = 18, CHIP_POSIX_LINUX = 999 } esp_chip_model_t;
#define CHIP_FEATURE_EMB_FLASH (1 << 0)
#define CHIP_FEATURE_WIFI_BGN  (1 << 1)
#define CHIP_FEATURE_BLE       (1 << 4)
#define CHIP_FEATURE_BT        (1 << 5)
#define CHIP_FEATURE_EMB_PSRAM (1 << 6)
typedef struct { esp_chip_model_t model; uint32_t features; uint16_t revision; uint8_t cores; } esp_chip_info_t;
static inline void esp_chip_info(esp_chip_info_t* info) {
  info->model = CHIP_POSIX_LINUX; info->features = 0; info->revision = 0; info->cores = 1;
}
