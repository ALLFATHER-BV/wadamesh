// SPDX-License-Identifier: GPL-3.0-or-later
// Fake BQ27220: the readings are plain fields a test sets; setNewCapacity() is
// counted and only "takes" when accept_capacity is true (a sealed gauge refuses).
#pragma once
#include <cstdint>
#include "Wire.h"
class GaugeBQ27220 {
public:
  bool answers = true, accept_capacity = true;
  int refreshes = 0, capacity_writes = 0;
  uint16_t design = 3000, voltage = 3900, soc = 76, remaining = 1100, full = 1480;
  uint16_t tte = 600, ttf = 0xFFFF, cycles = 12, soh = 99;
  int16_t current = -40, avg_power = -150;
  float temp_c = 25.0f;
  bool begin(TwoWire&) { return answers; }
  bool refresh() { ++refreshes; return answers; }
  uint16_t getVoltage() const { return voltage; }
  uint16_t getStateOfCharge() const { return soc; }
  int16_t  getCurrent() const { return current; }
  int16_t  getAveragePower() const { return avg_power; }
  uint16_t getRemainingCapacity() const { return remaining; }
  uint16_t getFullChargeCapacity() const { return full; }
  uint16_t getTimeToEmpty() const { return tte; }
  uint16_t getTimeToFull() const { return ttf; }
  float    getTemperature() const { return temp_c; }
  uint16_t getCycleCount() const { return cycles; }
  uint16_t getStateOfHealth() const { return soh; }
  uint16_t getDesignCapacity() const { return design; }
  bool setNewCapacity(uint16_t d, uint16_t) { ++capacity_writes; if (accept_capacity) design = d; return accept_capacity; }
};
