#pragma once
// Wi-Fi crash guard for the ESP32-P4 boards (T-Display P4, CrowPanel P4 10.1).
//
// Their Wi-Fi lives on an ESP32-C6 co-processor. With an old C6 firmware a unit can
// crash every time Wi-Fi starts, and Wi-Fi starts on every boot once a network is
// saved: a CrowPanel 10.1 "worked for a day" (no network yet), then crashed right
// after each boot. A panic now restarts the device, so without this it would loop.
//
// RTC no-init memory (it survives a panic or watchdog reset, not a power cut) records
// that Wi-Fi is running. A crash reset while it ran switches Wi-Fi off in the settings
// (and the boot says so) when the crash came within 60 s of starting it, or when it
// is the second crash with Wi-Fi on within 30 minutes. The user turns it back on to
// retry. With Wi-Fi and Bluetooth off the C6 is not started at all.
//
// Include in exactly one translation unit (each P4 project's main.cpp).
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <helpers/esp32/WifiRuntimeStore.h>

namespace wifi_crash_guard {

constexpr uint32_t kMagic = 0x57474431u;   // "WGD1"
constexpr uint32_t kEarlyMs = 60000;        // a crash this soon after starting Wi-Fi trips at once
constexpr uint32_t kCleanMs = 30u * 60000u; // this long with Wi-Fi on and no crash forgets a strike

RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint32_t s_armed;    // 1 = Wi-Fi is running
RTC_NOINIT_ATTR static uint32_t s_early;    // 1 = it started less than kEarlyMs ago
RTC_NOINIT_ATTR static uint32_t s_strikes;  // crash resets with Wi-Fi on, not yet forgotten
static uint32_t s_start_ms = 0;
static bool s_tripped = false;
static bool s_told = false;

// Call once at boot, after wifiConfigBegin() and before Wi-Fi can start.
inline void boot() {
  const esp_reset_reason_t r = esp_reset_reason();
  const bool crashed = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                       r == ESP_RST_WDT;
  if (s_magic != kMagic) { s_strikes = 0; s_early = 0; }   // power-on: no history
  if (s_magic == kMagic && s_armed == 1 && crashed) {
    if (s_strikes < 100) ++s_strikes;
    if (s_early == 1 || s_strikes >= 2) {
      s_tripped = true;
      s_strikes = 0;
      wifiConfigSetRadioEnabled(false);
      printf("[WIFI] crashed with Wi-Fi on (%s): Wi-Fi switched off\n",
             s_early == 1 ? "right after starting it" : "twice in 30 min");
    }
  }
  s_magic = kMagic;
  s_armed = 0;
  s_early = 0;
}

// Wi-Fi was just started (the first WiFi.begin of this boot, or after re-enabling).
inline void started() {
  s_armed = 1;
  s_early = 1;
  s_start_ms = millis() ? millis() : 1;
}

// Wi-Fi was switched off on purpose: nothing to guard.
inline void stopped() {
  s_armed = 0;
  s_early = 0;
  s_start_ms = 0;
}

// Call every loop pass. Clears the marker once Wi-Fi has run long enough, and returns
// true exactly once, a few seconds after boot, when the user should be told.
inline bool tick() {
  if (s_start_ms && s_armed) {
    const uint32_t up = millis() - s_start_ms;
    if (s_early && up >= kEarlyMs) s_early = 0;
    if (s_strikes && up >= kCleanMs) { s_strikes = 0; s_start_ms = millis() ? millis() : 1; }
  }
  if (s_tripped && !s_told && millis() > 5000) {
    s_told = true;
    return true;
  }
  return false;
}

}  // namespace wifi_crash_guard
