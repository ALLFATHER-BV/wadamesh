#pragma once
// Task watchdog: none on a host. Calls succeed; nothing ever times out.
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef __cplusplus
extern "C" {
#endif
static inline esp_err_t esp_task_wdt_init(uint32_t timeout_s, bool panic) { (void)timeout_s; (void)panic; return ESP_OK; }
static inline esp_err_t esp_task_wdt_deinit(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(TaskHandle_t t) { (void)t; return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(TaskHandle_t t) { (void)t; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_status(TaskHandle_t t) { (void)t; return ESP_ERR_NOT_FOUND; }
#ifdef __cplusplus
}
#endif
