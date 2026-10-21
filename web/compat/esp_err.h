#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC     0x109
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_INVALID_MAC     0x10B
#define ESP_ERR_NOT_FINISHED    0x10C
#ifdef __cplusplus
extern "C" {
#endif
static inline const char* esp_err_to_name(esp_err_t code) { return code == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
#ifdef __cplusplus
}
#endif
#define ESP_ERROR_CHECK(x) do { (void)(x); } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) (x)
