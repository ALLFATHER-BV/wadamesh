#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
  ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB,
  ESP_RST_JTAG
} esp_reset_reason_t;
uint32_t esp_random(void);                       // host CSPRNG (arc4random / getrandom)
void esp_fill_random(void* buf, size_t len);
void esp_restart(void);
static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
typedef void (*shutdown_handler_t)(void);
static inline esp_err_t esp_register_shutdown_handler(shutdown_handler_t h) { (void)h; return ESP_OK; }
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t* mac, esp_mac_type_t type);
static inline esp_err_t esp_efuse_mac_get_default(uint8_t* mac) { return esp_read_mac(mac, ESP_MAC_WIFI_STA); }
static inline const char* esp_get_idf_version(void) { return "host"; }
#ifdef __cplusplus
}
#endif
