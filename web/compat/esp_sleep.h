#pragma once
// Deep/light sleep: there is no sleep on a host. Power-off = exit the process.
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_ALL, ESP_SLEEP_WAKEUP_EXT0, ESP_SLEEP_WAKEUP_EXT1,
               ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_TOUCHPAD, ESP_SLEEP_WAKEUP_ULP, ESP_SLEEP_WAKEUP_GPIO,
               ESP_SLEEP_WAKEUP_UART } esp_sleep_source_t;
typedef esp_sleep_source_t esp_sleep_wakeup_cause_t;
typedef enum { ESP_EXT1_WAKEUP_ALL_LOW = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1, ESP_EXT1_WAKEUP_ANY_LOW = 2 } esp_sleep_ext1_wakeup_mode_t;
typedef enum { ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_DOMAIN_XTAL, ESP_PD_DOMAIN_MAX } esp_sleep_pd_domain_t;
typedef enum { ESP_PD_OPTION_OFF, ESP_PD_OPTION_ON, ESP_PD_OPTION_AUTO } esp_sleep_pd_option_t;
static inline esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level) { (void)gpio; (void)level; return ESP_OK; }
static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t mask, int mode) { (void)mask; (void)mode; return ESP_OK; }
static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { (void)us; return ESP_OK; }
static inline esp_err_t esp_sleep_enable_gpio_wakeup(void) { return ESP_OK; }
static inline esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t s) { (void)s; return ESP_OK; }
static inline esp_err_t esp_sleep_pd_config(esp_sleep_pd_domain_t d, esp_sleep_pd_option_t o) { (void)d; (void)o; return ESP_OK; }
static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return ESP_SLEEP_WAKEUP_UNDEFINED; }
static inline esp_err_t esp_light_sleep_start(void) { return ESP_OK; }
void esp_deep_sleep_start(void);   // arduino_host.cpp: ends the process
static inline void esp_deep_sleep(uint64_t us) { (void)us; esp_deep_sleep_start(); }
#ifdef __cplusplus
}
#endif
