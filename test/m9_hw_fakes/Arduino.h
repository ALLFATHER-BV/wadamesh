// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define ADC_11db 3

extern uint32_t g_test_ms;
inline uint32_t millis() { return g_test_ms; }
inline void delay(uint32_t ms) { g_test_ms += ms; }

struct PinTransition {
  int pin;
  int level;
  uint32_t at_ms;
};

extern int g_pin_mode[64];
extern int g_pin_level[64];
extern std::vector<PinTransition> g_pin_writes;
extern std::deque<uint32_t> g_adc_mv_queue;
extern uint32_t g_adc_default_mv;
extern int g_adc_resolution;
extern int g_adc_attenuation;

inline void pinMode(int pin, int mode) {
  if (pin >= 0 && pin < 64) g_pin_mode[pin] = mode;
}

inline void digitalWrite(int pin, int level) {
  if (pin >= 0 && pin < 64) {
    g_pin_level[pin] = level;
    g_pin_writes.push_back({pin, level, g_test_ms});
  }
}

inline int digitalRead(int pin) {
  if (pin >= 0 && pin < 64) return g_pin_level[pin];
  return LOW;
}

inline void analogReadResolution(int bits) { g_adc_resolution = bits; }
inline void analogSetPinAttenuation(int, int atten) { g_adc_attenuation = atten; }

inline uint32_t analogReadMilliVolts(int) {
  if (!g_adc_mv_queue.empty()) {
    uint32_t v = g_adc_mv_queue.front();
    g_adc_mv_queue.pop_front();
    return v;
  }
  return g_adc_default_mv;
}

class Stream {
public:
  std::string tx;
  std::string rx;
  size_t rx_pos = 0;
  int last_set_rx = -1;
  int last_set_tx = -1;
  int set_pins_calls = 0;
  uint32_t baud = 0;
  bool open = false;

  virtual ~Stream() {}
  virtual int available() { return rx_pos < rx.size() ? (int)(rx.size() - rx_pos) : 0; }
  virtual int read() { return rx_pos < rx.size() ? (uint8_t)rx[rx_pos++] : -1; }
  size_t write(uint8_t b) { tx.push_back((char)b); return 1; }
  size_t write(const uint8_t* p, size_t n) { tx.append((const char*)p, n); return n; }
  void print(const char* s) { if (s) tx += s; }
  void println(const char* s) { if (s) { tx += s; tx += "\n"; } }
  void printf(const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    tx += b;
  }
  void flush() {}
  void begin(uint32_t b) { baud = b; open = true; }
  void end() { open = false; }
  void setPins(int r, int t) {
    last_set_rx = r;
    last_set_tx = t;
    ++set_pins_calls;
  }
  size_t setRxBufferSize(size_t s) { return s; }
};

using HardwareSerial = Stream;
extern Stream Serial;
extern Stream Serial1;

typedef int gpio_num_t;
inline void gpio_hold_dis(gpio_num_t) {}
inline void gpio_hold_en(gpio_num_t) {}
inline void gpio_deep_sleep_hold_dis() {}
inline void gpio_deep_sleep_hold_en() {}
inline void rtc_gpio_pullup_en(gpio_num_t) {}
inline void rtc_gpio_pullup_dis(gpio_num_t) {}
inline void rtc_gpio_pulldown_en(gpio_num_t) {}
inline void rtc_gpio_pulldown_dis(gpio_num_t) {}
inline void rtc_gpio_hold_en(gpio_num_t) {}
inline void rtc_gpio_hold_dis(gpio_num_t) {}

enum {
  ESP_SLEEP_WAKEUP_ALL = 0,
  ESP_EXT1_WAKEUP_ALL_LOW = 0,
  ESP_EXT1_WAKEUP_ANY_HIGH = 1,
};
inline void esp_sleep_disable_wakeup_source(int) {}
inline void esp_sleep_enable_timer_wakeup(uint64_t) {}
inline void esp_sleep_enable_ext1_wakeup(uint64_t, int) {}
inline void esp_deep_sleep_start() {}

struct FakeRadioDriver {
  int power_off_calls = 0;
  void powerOff() { ++power_off_calls; }
};
struct FakeLocationProvider {
  int stop_calls = 0;
  virtual ~FakeLocationProvider() {}
  virtual void stop() { ++stop_calls; }
};
struct FakeSensorManager {
  FakeLocationProvider* provider = nullptr;
  FakeLocationProvider* getLocationProvider() { return provider; }
};
extern FakeRadioDriver radio_driver;
extern FakeSensorManager sensors;
