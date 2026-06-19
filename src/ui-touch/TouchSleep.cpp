#include "TouchSleep.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace touchSleep {
namespace {
  // When the gate passes (device parked: screen off, no companion client, WiFi+BLE
  // off, on battery, mesh idle) we THROTTLE the main loop with vTaskDelay instead of
  // entering esp_light_sleep.
  //
  // Why not real light sleep: manual esp_light_sleep_start() from the Arduino loop
  // task trips the Interrupt Watchdog in this build (dual-core, no PM framework,
  // INT_WDT on both cores @ 300 ms, always-on companion/LVGL/USB tasks) — reliably,
  // regardless of duration or wake-source handling. vTaskDelay is a standard scheduler
  // yield: the FreeRTOS idle task halts the CPU (WFI) between 1 ms ticks, trimming the
  // idle busy-spin draw, with zero watchdog/hang risk. The radio stays in RX; an
  // incoming packet is serviced on the next loop iteration (<= THROTTLE_MS late).
  constexpr uint32_t THROTTLE_MS = 50;

  uint64_t g_acc_idle_us = 0;     // cumulative time spent parked/throttled

  Hooks        g_hooks = {};
  TransitionCb g_transition = nullptr;
  bool         g_enabled = false;     // default OFF; driven from NVS via touchPrefsGetSleepIdle() at init
  bool         g_asleep_regime = false;
  uint32_t     g_cycle_count = 0;     // throttle cycles while parked (liveness counter)
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

void loopEnd(uint32_t /*now_ms*/) {
  const bool pass = gatePasses();
  if (!pass) {
    if (g_asleep_regime) { g_asleep_regime = false; emitTransition(false); } // sun — resumed activity
    return;
  }
  if (!g_asleep_regime) { g_asleep_regime = true; emitTransition(true); }    // moon — parked

  // Yield the CPU to the idle task for THROTTLE_MS (replaces esp_light_sleep_start —
  // see the note above). Standard FreeRTOS call: cannot starve a watchdog or hang.
  const uint64_t t0 = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(THROTTLE_MS));
  g_acc_idle_us += (uint64_t)(esp_timer_get_time() - t0);
  g_cycle_count++;
  g_last_reason = WakeReason::Timer;   // a throttle is a timed yield
}

bool       isSleeping()      { return g_asleep_regime; }   // true while parked/throttling
uint32_t   wakeCount()       { return g_cycle_count; }     // throttle cycles while parked
WakeReason lastWakeReason()  { return g_last_reason; }

uint8_t pctAsleep() {
  const uint64_t up = (uint64_t)esp_timer_get_time();
  if (up == 0) return 0;
  uint64_t pct = (g_acc_idle_us * 100ULL) / up;
  return pct > 100 ? 100 : (uint8_t)pct;
}

} // namespace touchSleep
