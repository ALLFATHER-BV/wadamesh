#pragma once
#include <stdint.h>

// Idle light-sleep controller for the T-Deck (ESP32-S3). Parks the CPU in light
// sleep between events while the SX1262 keeps listening; its DIO1 IRQ (GPIO45)
// wakes us on every packet. RAM / LVGL UI / mesh state all survive (this is
// light sleep, NOT deep sleep). See .notes/2026-06-18-light-sleep-rx-design.md.
//
// Decoupled from UI/mesh by predicate hooks supplied by the integration layer
// (UITask.cpp), so this module pulls in no UI/mesh/LVGL headers.
namespace touchSleep {

enum class WakeReason : uint8_t { None, Timer, Packet, Touch, Button, Other };

struct Hooks {
  bool     (*screenOff)();      // display is off
  bool     (*noClient)();       // no companion client on any transport
  bool     (*wifiOff)();        // WiFi fully off
  bool     (*bleOff)();         // BLE fully off
  bool     (*onBattery)();      // running on battery (no USB / charge source)
  bool     (*meshIdle)();       // radio not mid-RX AND send queue empty
  // ms until the soonest wake-forcing deadline (advert / clock alarm),
  // or UINT32_MAX when nothing must wake us:
  uint32_t (*nextWakeForcingDueMs)(uint32_t now_ms);
  uint32_t (*epochNow)();       // wall-clock epoch seconds (for the event log)
};

// regime transition: entering==true  -> went to sleep      (moon)
//                     entering==false -> woke to activity   (sun)
using TransitionCb = void (*)(uint32_t epoch, bool entering);

void begin(const Hooks& hooks);   // install hooks; arm static GPIO wake config once
void onTransition(TransitionCb cb);
void setEnabled(bool on);         // feature toggle (driven by the NVS pref)
bool enabled();

void loopEnd(uint32_t now_ms);    // call at the very END of the main loop()

// instrumentation (read by the settings / diag UI)
bool       isSleeping();          // currently in the asleep regime
uint32_t   wakeCount();           // cumulative meaningful wakes
WakeReason lastWakeReason();
uint8_t    pctAsleep();           // % wall-time asleep since boot (0..100)

} // namespace touchSleep
