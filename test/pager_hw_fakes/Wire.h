// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
struct TwoWire {
  uint32_t clock = 100000;
  uint32_t max_clock_seen = 100000;
  void begin(int, int) {}
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission() { return 0; }
  uint32_t getClock() { return clock; }
  void setClock(uint32_t c) { clock = c; if (c > max_clock_seen) max_clock_seen = c; }
};
extern TwoWire Wire;
