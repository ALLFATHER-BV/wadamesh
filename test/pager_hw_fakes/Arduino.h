// SPDX-License-Identifier: GPL-3.0-or-later
// Host fakes for the T-LoRa Pager chip-driver tests (test_pager_*.cpp): just enough
// Arduino / ESP-IDF surface for the variant's own .cpp files to compile and run.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

extern uint32_t testMillis;
inline uint32_t millis() { return testMillis; }
inline void delay(uint32_t ms) { testMillis += ms; }

struct TestSerial {
  std::string output;
  void printf(const char* fmt, ...) {
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); output += b;
  }
  void println(const char* s) { output += s; output += "\n"; }
  void flush() {}
};
extern TestSerial Serial;

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLDOWN 0x09
#ifndef SDA
#define SDA 3
#define SCL 2
#endif
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}

typedef int gpio_num_t;
inline void gpio_hold_dis(gpio_num_t) {}
inline void gpio_hold_en(gpio_num_t) {}
inline void gpio_deep_sleep_hold_dis() {}
inline void gpio_deep_sleep_hold_en() {}
enum { RTC_GPIO_MODE_INPUT_ONLY = 0 };
inline void rtc_gpio_set_direction(gpio_num_t, int) {}
inline void rtc_gpio_pulldown_en(gpio_num_t) {}
inline void rtc_gpio_hold_en(gpio_num_t) {}
inline void rtc_gpio_hold_dis(gpio_num_t) {}
inline void rtc_gpio_deinit(gpio_num_t) {}

typedef int esp_reset_reason_t;
enum { ESP_RST_POWERON = 1, ESP_RST_DEEPSLEEP = 8 };
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
inline long esp_sleep_get_ext1_wakeup_status() { return 0; }
enum { ESP_PD_DOMAIN_RTC_PERIPH = 0, ESP_PD_OPTION_ON = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1 };
inline void esp_sleep_pd_config(int, int) {}
inline void esp_sleep_enable_ext1_wakeup(uint64_t, int) {}
inline void esp_sleep_enable_timer_wakeup(uint64_t) {}
inline void esp_deep_sleep_start() {}
