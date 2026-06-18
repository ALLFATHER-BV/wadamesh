#include "TouchSleep.h"

namespace touchSleep {
namespace {
  Hooks        g_hooks = {};
  TransitionCb g_transition = nullptr;
  bool         g_enabled = false;     // Phase 2 drives this from the NVS pref
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

void loopEnd(uint32_t /*now_ms*/) {
  const bool pass = gatePasses();
  if (!pass) {
    if (g_asleep_regime) { g_asleep_regime = false; emitTransition(false); } // sun
    return;
  }
  if (!g_asleep_regime) { g_asleep_regime = true; emitTransition(true); }    // moon
  // Task 2 replaces the body below with the real esp_light_sleep_start() path.
}

bool       isSleeping()      { return g_asleep_regime; }
uint32_t   wakeCount()       { return g_wake_count; }
WakeReason lastWakeReason()  { return g_last_reason; }
uint8_t    pctAsleep()       { return 0; }

} // namespace touchSleep
