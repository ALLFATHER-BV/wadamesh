#pragma once
#include "esp_err.h"
static inline esp_err_t rtc_gpio_pullup_en(int gpio) { (void)gpio; return ESP_OK; }
static inline esp_err_t rtc_gpio_pullup_dis(int gpio) { (void)gpio; return ESP_OK; }
static inline esp_err_t rtc_gpio_pulldown_en(int gpio) { (void)gpio; return ESP_OK; }
static inline esp_err_t rtc_gpio_pulldown_dis(int gpio) { (void)gpio; return ESP_OK; }
static inline esp_err_t rtc_gpio_hold_en(int gpio) { (void)gpio; return ESP_OK; }
static inline esp_err_t rtc_gpio_hold_dis(int gpio) { (void)gpio; return ESP_OK; }
static inline esp_err_t rtc_gpio_isolate(int gpio) { (void)gpio; return ESP_OK; }
