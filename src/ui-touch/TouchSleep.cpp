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

  // Per-condition attribution of the time the gate was SHUT. "asleep 15%" on its own
  // cannot distinguish "parks are too short" from "the device was hardly ever
  // eligible", which is the whole question when someone reports the saver doing
  // nothing (#465). Charge the wall time between checks to whichever condition was
  // blocking at that moment.
  uint64_t g_blocked_us[(int)Blocker::Count] = { 0 };
  uint64_t g_measured_us  = 0;    // total time actually attributed (blocked + eligible)
  uint64_t g_last_check_us = 0;   // when loopEnd last finished
  // A gap longer than this means the loop was stalled somewhere unrelated (a long
  // flash write, a modal, a boot pause); charging it to a condition would be a lie.
  constexpr uint64_t MAX_ATTRIBUTABLE_US = 5ULL * 1000000ULL;

  Hooks        g_hooks = {};
  TransitionCb g_transition = nullptr;
  bool         g_enabled = false;     // default OFF; driven from NVS via touchPrefsGetSleepIdle() at init
  bool         g_asleep_regime = false;
  uint32_t     g_cycle_count = 0;     // throttle cycles while parked (liveness counter)
  WakeReason   g_last_reason = WakeReason::None;

  // -1 = the gate passes; otherwise the (int)Blocker holding it shut.
  int gateBlocker() {
    if (!g_enabled) return (int)Blocker::Disabled;
    if (!g_hooks.screenOff || !g_hooks.screenOff()) return (int)Blocker::ScreenOn;
    if (!g_hooks.noClient  || !g_hooks.noClient())  return (int)Blocker::ClientConnected;
    if (!g_hooks.wifiOff   || !g_hooks.wifiOff())   return (int)Blocker::WifiOn;
    if (!g_hooks.bleOff    || !g_hooks.bleOff())    return (int)Blocker::BleOn;
    if (!g_hooks.onBattery || !g_hooks.onBattery()) return (int)Blocker::UsbPower;
    if (!g_hooks.meshIdle  || !g_hooks.meshIdle())  return (int)Blocker::MeshBusy;
    return -1;
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
  const uint64_t t_enter = esp_timer_get_time();
  const int blocker = gateBlocker();
  // Charge the time since the last check. g_last_check_us is stamped AFTER the park
  // below, so a completed park is never mis-attributed to the next blocker.
  if (g_last_check_us) {
    const uint64_t dt = t_enter - g_last_check_us;
    if (dt < MAX_ATTRIBUTABLE_US) {
      g_measured_us += dt;
      if (blocker >= 0) g_blocked_us[blocker] += dt;
    }
  }
  if (blocker >= 0) {
    if (g_asleep_regime) { g_asleep_regime = false; emitTransition(false); } // sun — resumed activity
    g_last_check_us = esp_timer_get_time();
    return;
  }
  if (!g_asleep_regime) { g_asleep_regime = true; emitTransition(true); }    // moon — parked

  // Yield the CPU to the idle task, capped by the earliest retry/UI deadline.
  // This is the wake timer for these builds: unlike manual esp_light_sleep_start,
  // a timed FreeRTOS block remains watchdog-safe and resumes the loop on schedule.
  uint32_t park_ms = THROTTLE_MS;
  if (g_hooks.nextWakeForcingDueMs) {
    const uint32_t wake_due_ms = g_hooks.nextWakeForcingDueMs(now_ms);
    if (wake_due_ms < park_ms) park_ms = wake_due_ms;
  }
  if (park_ms == 0) {
    g_last_reason = WakeReason::Timer;
    g_last_check_us = esp_timer_get_time();
    return;
  }

  const uint64_t t0 = esp_timer_get_time();
  TickType_t park_ticks = pdMS_TO_TICKS(park_ms);
  if (park_ticks == 0) park_ticks = 1;
  vTaskDelay(park_ticks);
  g_acc_idle_us += (uint64_t)(esp_timer_get_time() - t0);
  g_cycle_count++;
  g_last_reason = WakeReason::Timer;   // a throttle is a timed yield
  g_last_check_us = esp_timer_get_time();
}

bool       isSleeping()      { return g_asleep_regime; }   // true while parked/throttling
uint32_t   wakeCount()       { return g_cycle_count; }     // throttle cycles while parked
WakeReason lastWakeReason()  { return g_last_reason; }

uint8_t blockedPct(Blocker b) {
  const int i = (int)b;
  if (i < 0 || i >= (int)Blocker::Count || g_measured_us == 0) return 0;
  const uint64_t pct = (g_blocked_us[i] * 100ULL) / g_measured_us;
  return pct > 100 ? 100 : (uint8_t)pct;
}

Blocker topBlocker() {
  int best = -1;
  uint64_t best_us = 0;
  for (int i = 0; i < (int)Blocker::Count; ++i)
    if (g_blocked_us[i] > best_us) { best_us = g_blocked_us[i]; best = i; }
  return best < 0 ? Blocker::Count : (Blocker)best;
}

uint8_t pctAsleep() {
  const uint64_t up = (uint64_t)esp_timer_get_time();
  if (up == 0) return 0;
  uint64_t pct = (g_acc_idle_us * 100ULL) / up;
  return pct > 100 ? 100 : (uint8_t)pct;
}

} // namespace touchSleep
