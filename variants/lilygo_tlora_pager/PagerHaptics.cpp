// SPDX-License-Identifier: GPL-3.0-or-later
#include "PagerHaptics.h"

#include <Arduino.h>
#include <Wire.h>
#include <SensorDRV2605.hpp>

// SensorLib's DRV2605 driver does the probe and the ERM open-loop setup. The two
// things it has no call for, standby and reading GO back, go through its own
// register access (comm is protected, hence the subclass).
namespace {
class PagerDrv : public SensorDRV2605 {
public:
  static constexpr uint8_t kRegMode = 0x01;   // bit6 STANDBY, [2:0] mode
  static constexpr uint8_t kRegGo   = 0x0C;   // bit0 GO, cleared by the chip at the end
  void standby() { if (comm) comm->writeRegister(kRegMode, (uint8_t)0x40); }
  bool playing() {
    const int go = comm ? comm->readRegister(kRegGo) : -1;
    return go >= 0 && (go & 0x01);
  }
};
}  // namespace

#define DRV_WAIT(ms10) (uint8_t)(0x80 | (ms10))   // sequencer slot: pause of ms10 * 10 ms

static PagerDrv s_drv;
static bool     s_present = false;
static bool     s_awake = false;       // out of standby, a pattern may be running
static uint32_t s_check_ms = 0;        // when to look at GO next

void pagerHapticsBegin() {
  s_present = s_drv.begin(Wire);       // checks the chip id, sets ERM open loop
  if (s_present) {
    s_drv.selectLibrary(1);            // ERM library A
    s_drv.standby();
  }
  s_awake = false;
  Serial.printf("[BOOT] drv2605 %s\n", s_present ? "ok" : "PROBE FAILED");
}

bool pagerHapticsPresent() { return s_present; }

void pagerHapticsPlay(PagerHaptic kind) {
  if (!s_present) return;
  // Library A effect ids: 1 strong click, 14 strong buzz. The key/knob click is
  // effect 1 too: the sharp ticks (24-26) are too short for this ERM motor to
  // spin up, and were not felt at all on hardware.
  static const uint8_t k_msg[]  = { 14, 0 };
  static const uint8_t k_dm[]   = { 14, DRV_WAIT(12), 14, 0 };
  static const uint8_t k_men[]  = { 1, DRV_WAIT(8), 1, DRV_WAIT(8), 14, 0 };
  static const uint8_t k_tick[] = { 1, 0 };
  const uint8_t* seq = k_msg;
  size_t n = sizeof k_msg;
  switch (kind) {
    case PagerHaptic::Message: seq = k_msg;  n = sizeof k_msg;  break;
    case PagerHaptic::Direct:  seq = k_dm;   n = sizeof k_dm;   break;
    case PagerHaptic::Mention: seq = k_men;  n = sizeof k_men;  break;
    case PagerHaptic::Tick:    seq = k_tick; n = sizeof k_tick; break;
  }
  s_drv.setMode(SensorDRV2605::MODE_INTTRIG);   // wake, internal trigger
  s_drv.stop();      // GO=1 again does not restart a running sequence, and the
                     // sequencer reads the slots live: stop it before rewriting
  for (size_t i = 0; i < n && i < 8; ++i) s_drv.setWaveform((uint8_t)i, seq[i]);
  s_drv.run();
  s_awake = true;
  s_check_ms = millis() + 250;
}

static void playSeq(const uint8_t* seq, size_t n) {
  if (!s_present) return;
  s_drv.setMode(SensorDRV2605::MODE_INTTRIG);
  s_drv.stop();
  for (size_t i = 0; i < 8; ++i) s_drv.setWaveform((uint8_t)i, i < n ? seq[i] : 0);
  s_drv.run();
  s_awake = true;
  s_check_ms = millis() + 250;
}

void pagerHapticsPattern(uint8_t pattern, uint8_t strength) {
  if (strength > 2) strength = 2;
  // Library A: 14 strong buzz (100 %), 48 / 49 buzz at 80 / 60 %; clicks 1 / 2 / 3
  // at 100 / 60 / 30 %. The motor's own inertia makes anything weaker than that
  // hard to feel, so "light" stops at 60 %.
  static const uint8_t k_buzz[3]  = { 49, 48, 14 };
  static const uint8_t k_click[3] = { 2, 1, 1 };
  const uint8_t b = k_buzz[strength], c = k_click[strength];
  uint8_t seq[8] = { 0 };
  size_t n = 0;
  switch (pattern) {
    case PAGER_VIB_ONCE:   seq[0] = b; n = 1; break;
    case PAGER_VIB_TWICE:  seq[0] = b; seq[1] = DRV_WAIT(12); seq[2] = b; n = 3; break;
    case PAGER_VIB_THRICE: seq[0] = b; seq[1] = DRV_WAIT(8); seq[2] = b; seq[3] = DRV_WAIT(8); seq[4] = b; n = 5; break;
    case PAGER_VIB_LONG:   seq[0] = b; seq[1] = b; seq[2] = b; seq[3] = b; n = 4; break;
    case PAGER_VIB_PULSE:  // a heartbeat: two quick clicks, a rest, two more
      seq[0] = c; seq[1] = DRV_WAIT(10); seq[2] = c; seq[3] = DRV_WAIT(40);
      seq[4] = c; seq[5] = DRV_WAIT(10); seq[6] = c; n = 7; break;
    default: return;      // PAGER_VIB_OFF
  }
  playSeq(seq, n);
}

void pagerHapticsClick(uint8_t level) {
  if (!level) return;
  const uint8_t seq[1] = { (uint8_t)(level >= 2 ? 1 : 2) };   // strong click 100 % / 60 %
  playSeq(seq, 1);
}

void pagerHapticsIdleTick() {
  if (!s_awake) return;
  const uint32_t now = millis();
  if ((int32_t)(now - s_check_ms) < 0) return;
  if (s_drv.playing()) { s_check_ms = now + 100; return; }
  s_drv.standby();
  s_awake = false;
}
