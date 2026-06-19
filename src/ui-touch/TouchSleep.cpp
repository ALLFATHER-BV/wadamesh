#include "TouchSleep.h"
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>

// PIN_TOUCH_INT is defined as a compile-unit fallback in TDeckTouch.cpp (not in
// platformio.ini or a shared header). Mirror the same #ifndef guard here so
// the T-Deck wake-pin table compiles without depending on include order.
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 16
#endif

namespace touchSleep {
namespace {
  constexpr uint32_t MIN_SLEEP_MS = 50;
  constexpr uint32_t MAX_SLEEP_MS = 300000;   // 5 min ceiling (tunable)

  // Confirmed T-Deck wake pins (from platformio.ini / TDeckTouch.cpp):
  //   DIO1  GPIO45 — SX1262 RxDone, active-HIGH on packet.
  //   BTN   GPIO0  — user button (trackball click), active-LOW.
  //   Touch GPIO16 — GT911 INT, asserts LOW when touch data is ready (INPUT poll).
  // All three are inside HAS_TDECK_GT911 because the Heltec V4 has no SD, no GT911,
  // and the sleep body must not compile on it either.
  struct WakePin { gpio_num_t pin; gpio_int_type_t level; };
  const WakePin kWakePins[] = {
  #if defined(HAS_TDECK_GT911)
    { (gpio_num_t)P_LORA_DIO_1,  GPIO_INTR_HIGH_LEVEL },   // SX1262 RxDone -> GPIO45
    { (gpio_num_t)PIN_USER_BTN,  GPIO_INTR_LOW_LEVEL  },   // user button   -> GPIO0
    { (gpio_num_t)PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL  },   // GT911 INT     -> GPIO16
  #endif
  };

  // Serialises sleep entry and lets us disable the level wakeups on wake while
  // interrupts are still masked (see loopEnd) — mirrors MeshCore ESP32Board.
  portMUX_TYPE s_sleep_mux = portMUX_INITIALIZER_UNLOCKED;

  uint64_t g_acc_sleep_us = 0;

  Hooks        g_hooks = {};
  TransitionCb g_transition = nullptr;
  bool         g_enabled = false;     // default OFF; driven from NVS via touchPrefsGetSleepIdle() at init
  bool         g_asleep_regime = false;
  uint32_t     g_wake_count = 0;
  WakeReason   g_last_reason = WakeReason::None;

  bool gatePasses() {
    if (!g_enabled) return false;
    if (!g_hooks.screenOff || !g_hooks.screenOff()) return false;
    if (!g_hooks.noClient  || !g_hooks.noClient())  return false;
    if (!g_hooks.wifiOff   || !g_hooks.wifiOff())   return false;
    if (!g_hooks.bleOff    || !g_hooks.bleOff())    return false;
    if (!g_hooks.onBattery || !g_hooks.onBattery()) return false;
    if (!g_hooks.meshIdle  || !g_hooks.meshIdle())  return false;
    return true;
  }
  void emitTransition(bool entering) {
    if (g_transition && g_hooks.epochNow) g_transition(g_hooks.epochNow(), entering);
  }
} // namespace

void begin(const Hooks& hooks) { g_hooks = hooks; }
void onTransition(TransitionCb cb) { g_transition = cb; }
void setEnabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

void loopEnd(uint32_t now_ms) {
  const bool pass = gatePasses();
  if (!pass) {
    if (g_asleep_regime) { g_asleep_regime = false; emitTransition(false); } // sun
    return;
  }
  if (!g_asleep_regime) { g_asleep_regime = true; emitTransition(true); }    // moon

#if defined(HAS_TDECK_GT911)
  // budget = time to soonest wake-forcing deadline, clamped.
  // tsNextWakeForcingDueMs returns UINT32_MAX when no deadline is known; in that
  // case the MAX_SLEEP_MS ceiling (5 min) governs — which also matches the
  // default signal-probe advert cadence, so the device wakes at least that often.
  uint32_t budget = g_hooks.nextWakeForcingDueMs ? g_hooks.nextWakeForcingDueMs(now_ms)
                                                 : UINT32_MAX;
  if (budget > MAX_SLEEP_MS) budget = MAX_SLEEP_MS;
  if (budget < MIN_SLEEP_MS) budget = MIN_SLEEP_MS;

  // Keep the flash/PSRAM power rail alive across light sleep (LVGL buffers live
  // in PSRAM; powering down VDD_SPI would corrupt them).
  esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);
  esp_sleep_enable_timer_wakeup((uint64_t)budget * 1000ULL);
  esp_task_wdt_reset();   // feed the Task WDT before sleeping (no-op if loopTask isn't subscribed)

  // --- Flood-safe light-sleep entry (mirrors MeshCore ESP32Board::sleep) ---
  // The wake pins are LEVEL-triggered. If a pin is still at its trigger level
  // when esp_light_sleep_start() returns (e.g. DIO1 stays HIGH until the LoRa
  // packet is read), the level interrupt FLOODS the CPU and the Interrupt
  // Watchdog (300 ms) fires -> "Int reset". So we run inside a critical section
  // and disable the level wakeups on wake BEFORE interrupts are re-enabled.
  portENTER_CRITICAL(&s_sleep_mux);

  // Skip the sleep entirely if a wake pin is already at its trigger level —
  // sleeping would return instantly and thrash (e.g. a LoRa packet pending on
  // DIO1, or a finger still on the screen). We'll retry next loop.
  bool already_triggered = false;
  for (const auto& w : kWakePins) {
    const int lvl = gpio_get_level(w.pin);
    if ((w.level == GPIO_INTR_HIGH_LEVEL && lvl) ||
        (w.level == GPIO_INTR_LOW_LEVEL  && !lvl)) { already_triggered = true; break; }
  }
  if (already_triggered) { portEXIT_CRITICAL(&s_sleep_mux); return; }

  esp_sleep_enable_gpio_wakeup();
  for (const auto& w : kWakePins) gpio_wakeup_enable(w.pin, w.level);

  const uint64_t t0 = esp_timer_get_time();
  esp_light_sleep_start();
  const uint64_t slept = esp_timer_get_time() - t0;

  // Capture the wake cause + pin levels, then disable the level wakeups while
  // still masked so the (still-asserted) trigger level cannot flood interrupts.
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool dio1_hi = gpio_get_level((gpio_num_t)P_LORA_DIO_1);
  const bool btn_lo  = !gpio_get_level((gpio_num_t)PIN_USER_BTN);
  for (const auto& w : kWakePins) gpio_wakeup_disable(w.pin);

  portEXIT_CRITICAL(&s_sleep_mux);

  esp_task_wdt_reset();   // feed again on resume
  g_acc_sleep_us += slept;

  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: g_last_reason = WakeReason::Timer; break;
    case ESP_SLEEP_WAKEUP_GPIO:
      // DIO1 high => a LoRa packet is waiting; button low => user button; else touch
      g_last_reason = dio1_hi ? WakeReason::Packet : (btn_lo ? WakeReason::Button : WakeReason::Touch);
      break;
    default: g_last_reason = WakeReason::Other; break;
  }
  if (g_last_reason != WakeReason::Timer) g_wake_count++;
#endif
}

bool       isSleeping()      { return g_asleep_regime; }
uint32_t   wakeCount()       { return g_wake_count; }
WakeReason lastWakeReason()  { return g_last_reason; }

uint8_t pctAsleep() {
  const uint64_t up = (uint64_t)esp_timer_get_time();
  if (up == 0) return 0;
  uint64_t pct = (g_acc_sleep_us * 100ULL) / up;
  return pct > 100 ? 100 : (uint8_t)pct;
}

} // namespace touchSleep
