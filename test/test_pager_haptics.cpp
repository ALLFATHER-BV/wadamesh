// SPDX-License-Identifier: GPL-3.0-or-later
// The T-LoRa Pager's vibration patterns (PagerHaptics.cpp) on a fake DRV2605 register
// file: the waveform slots each pattern and strength writes, every slot rewritten so
// a shorter pattern does not inherit the tail of a longer one, the chip woken for a
// pattern and parked in standby once GO clears, and nothing at all without the chip.
//
// Build: g++ -std=c++17 -I test/pager_hw_fakes -I src test/test_pager_haptics.cpp
#include <cassert>
#include <cstdio>
#include <vector>
#include "pager_hw_fakes/Arduino.h"
#include "pager_hw_fakes/Wire.h"
#include "pager_hw_fakes/SensorDRV2605.hpp"
uint32_t testMillis = 1000;
TestSerial Serial;
TwoWire Wire;
FakeDrvComm g_drv;
bool g_drv_present = true;
#include "../variants/lilygo_tlora_pager/PagerHaptics.cpp"

static std::vector<uint8_t> slots() {
  std::vector<uint8_t> v;
  for (int i = 0; i < 8; ++i) v.push_back(g_drv.reg[0x04 + i]);
  return v;
}
static uint8_t W(int ms10) { return (uint8_t)(0x80 | ms10); }

int main() {
  pagerHapticsBegin();
  assert(pagerHapticsPresent());
  assert(g_drv.reg[0x03] == 1);            // ERM library A
  assert(g_drv.reg[0x01] == 0x40);         // parked in standby after setup

  // Strength picks the buzz effect: 49 (60 %), 48 (80 %), 14 (100 %).
  const uint8_t buzz[3] = { 49, 48, 14 };
  for (uint8_t s = 0; s < 3; ++s) {
    const uint8_t b = buzz[s];
    pagerHapticsPattern(PAGER_VIB_ONCE, s);
    assert((slots() == std::vector<uint8_t>{ b, 0, 0, 0, 0, 0, 0, 0 }));
    pagerHapticsPattern(PAGER_VIB_TWICE, s);
    assert((slots() == std::vector<uint8_t>{ b, W(12), b, 0, 0, 0, 0, 0 }));
    pagerHapticsPattern(PAGER_VIB_THRICE, s);
    assert((slots() == std::vector<uint8_t>{ b, W(8), b, W(8), b, 0, 0, 0 }));
    pagerHapticsPattern(PAGER_VIB_LONG, s);
    assert((slots() == std::vector<uint8_t>{ b, b, b, b, 0, 0, 0, 0 }));
  }
  pagerHapticsPattern(PAGER_VIB_PULSE, 0);
  assert((slots() == std::vector<uint8_t>{ 2, W(10), 2, W(40), 2, W(10), 2, 0 }));
  pagerHapticsPattern(PAGER_VIB_PULSE, 2);
  assert((slots() == std::vector<uint8_t>{ 1, W(10), 1, W(40), 1, W(10), 1, 0 }));
  // Strength past the end is clamped, not an out-of-bounds read.
  pagerHapticsPattern(PAGER_VIB_ONCE, 7);
  assert(slots()[0] == 14);
  // A short pattern after a long one: the long one's tail is gone.
  pagerHapticsPattern(PAGER_VIB_PULSE, 2);
  pagerHapticsPattern(PAGER_VIB_ONCE, 2);
  assert((slots() == std::vector<uint8_t>{ 14, 0, 0, 0, 0, 0, 0, 0 }));
  assert(g_drv.reg[0x01] == 0x00 && g_drv.reg[0x0C] == 1);   // awake, internal trigger, GO

  // PAGER_VIB_OFF writes nothing.
  const int w0 = g_drv.writes;
  pagerHapticsPattern(PAGER_VIB_OFF, 2);
  pagerHapticsClick(0);
  assert(g_drv.writes == w0);

  // Key click: 100 % (effect 1) strong, 60 % (effect 2) light.
  pagerHapticsClick(2); assert(slots()[0] == 1 && slots()[1] == 0);
  pagerHapticsClick(1); assert(slots()[0] == 2);

  // Standby: not before the first 250 ms look, not while GO is still set, then once.
  pagerHapticsIdleTick();
  assert(g_drv.reg[0x01] == 0x00);
  testMillis += 300;
  pagerHapticsIdleTick();                  // GO still 1: still playing
  assert(g_drv.reg[0x01] == 0x00);
  g_drv.reg[0x0C] = 0;                     // the chip finished the sequence
  testMillis += 50;
  pagerHapticsIdleTick();                  // re-checks only every 100 ms
  assert(g_drv.reg[0x01] == 0x00);
  testMillis += 60;
  pagerHapticsIdleTick();
  assert(g_drv.reg[0x01] == 0x40);
  const int w1 = g_drv.writes;
  testMillis += 1000;
  pagerHapticsIdleTick();                  // parked: no more bus traffic
  assert(g_drv.writes == w1);

  // The legacy kinds still map to their sequences.
  pagerHapticsPlay(PagerHaptic::Mention);
  assert((slots() == std::vector<uint8_t>{ 1, W(8), 1, W(8), 14, 0, 0, 0 }));

  // No chip: every call is a no-op (and none touches a null comm).
  g_drv = FakeDrvComm{};
  g_drv_present = false;
  pagerHapticsBegin();
  assert(!pagerHapticsPresent());
  pagerHapticsPattern(PAGER_VIB_THRICE, 2);
  pagerHapticsClick(2);
  pagerHapticsPlay(PagerHaptic::Direct);
  testMillis += 1000;
  pagerHapticsIdleTick();
  assert(g_drv.writes == 0);
  std::printf("test_pager_haptics: OK\n");
  return 0;
}
