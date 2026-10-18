#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
static inline esp_err_t gpio_hold_en(gpio_num_t g) { (void)g; return ESP_OK; }
static inline esp_err_t gpio_hold_dis(gpio_num_t g) { (void)g; return ESP_OK; }
static inline void gpio_deep_sleep_hold_en(void) {}
static inline void gpio_deep_sleep_hold_dis(void) {}
static inline esp_err_t gpio_reset_pin(gpio_num_t g) { (void)g; return ESP_OK; }
