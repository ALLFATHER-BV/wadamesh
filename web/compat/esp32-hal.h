#pragma once
// esp32-hal: timing, GPIO, PSRAM and core-pinning helpers on a host.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "esp32-hal-log.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"   // esp_random(): arduino-esp32 pulls it in through esp32-hal.h too

#ifdef __cplusplus
extern "C" {
#endif

unsigned long millis(void);           // since process start (monotonic)
unsigned long micros(void);
void delay(uint32_t ms);              // sleeps the calling thread
void delayMicroseconds(uint32_t us);
void yield(void);

#define LOW 0x0
#define HIGH 0x1
#define INPUT 0x01
#define OUTPUT 0x03
#define PULLUP 0x04
#define INPUT_PULLUP 0x05
#define PULLDOWN 0x08
#define INPUT_PULLDOWN 0x09
#define OPEN_DRAIN 0x10
#define OUTPUT_OPEN_DRAIN 0x12
#define ANALOG 0xC0

// GPIO: there are no pins. Writes are dropped, reads return LOW / 0.
static inline void pinMode(uint8_t pin, uint8_t mode) { (void)pin; (void)mode; }
static inline void digitalWrite(uint8_t pin, uint8_t val) { (void)pin; (void)val; }
static inline int digitalRead(uint8_t pin) { (void)pin; return LOW; }
static inline uint16_t analogRead(uint8_t pin) { (void)pin; return 0; }
static inline uint32_t analogReadMilliVolts(uint8_t pin) { (void)pin; return 0; }
static inline void analogReadResolution(uint8_t bits) { (void)bits; }
static inline void analogWrite(uint8_t pin, int value) { (void)pin; (void)value; }
static inline uint32_t ledcSetup(uint8_t ch, uint32_t freq, uint8_t res) { (void)ch; (void)res; return freq; }
static inline void ledcAttachPin(uint8_t pin, uint8_t ch) { (void)pin; (void)ch; }
static inline void ledcDetachPin(uint8_t pin) { (void)pin; }
static inline void ledcWrite(uint8_t ch, uint32_t duty) { (void)ch; (void)duty; }
static inline uint32_t ledcWriteTone(uint8_t ch, uint32_t freq) { (void)ch; return freq; }
static inline void attachInterrupt(uint8_t pin, void (*cb)(void), int mode) { (void)pin; (void)cb; (void)mode; }
static inline void detachInterrupt(uint8_t pin) { (void)pin; }

// PSRAM: the host has plenty of memory; "PSRAM" is just the heap.
static inline bool psramFound(void) { return true; }
static inline bool psramInit(void) { return true; }
static inline void* ps_malloc(size_t n) { return malloc(n); }
static inline void* ps_calloc(size_t n, size_t s) { return calloc(n, s); }
static inline void* ps_realloc(void* p, size_t n) { return realloc(p, n); }

static inline uint32_t getCpuFrequencyMhz(void) { return 240; }
static inline bool setCpuFrequencyMhz(uint32_t mhz) { (void)mhz; return true; }
static inline uint32_t getXtalFrequencyMhz(void) { return 40; }
static inline uint32_t getApbFrequency(void) { return 80000000; }

// Task watchdog helpers: there is no hardware watchdog to feed.
static inline void enableLoopWDT(void) {}
static inline void disableLoopWDT(void) {}
static inline void feedLoopWDT(void) {}
static inline void enableCore0WDT(void) {}
static inline void disableCore0WDT(void) {}
static inline void enableCore1WDT(void) {}
static inline void disableCore1WDT(void) {}

static inline int temperatureRead(void) { return 0; }
const char* pathToFileName(const char* path);   // arduino_host.cpp

#ifdef __cplusplus
}
#endif
