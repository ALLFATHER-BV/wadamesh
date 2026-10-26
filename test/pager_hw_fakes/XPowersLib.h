// SPDX-License-Identifier: GPL-3.0-or-later
// Fake XPowersLib PowersBQ25896, register level for the bits the Pager code relies
// on. The bit positions are XPowersLib 0.3.3's (src/PowersBQ25896.tpp):
// REG03 bit4 CHG_CONFIG (enable/disableCharge), REG09 bit5 BATFET_DIS (shutdown),
// REG09 bit3 BATFET_DLY (setTurnOffDelay). REG0B is the status a test sets.
#pragma once
#include <cstdint>
#include "Wire.h"
class PowersBQ25896 {
public:
  bool present = true;
  uint8_t reg[0x15] = {};
  int enable_calls = 0, disable_calls = 0, shutdown_calls = 0;
  uint16_t ichg_ma = 2048, vreg_mv = 4208;   // the chip's power-on defaults
  bool init(TwoWire&, int, int, uint8_t) { return present; }
  int readRegister(uint8_t r) { return present && r < sizeof reg ? reg[r] : -1; }
  void enableCharge()  { ++enable_calls;  reg[0x03] |= 0x10; }
  void disableCharge() { ++disable_calls; reg[0x03] &= (uint8_t)~0x10; }
  bool chargeEnabled() const { return reg[0x03] & 0x10; }
  bool setChargerConstantCurr(uint16_t ma) { if (ma % 64) return false; ichg_ma = ma; return true; }
  bool setChargeTargetVoltage(uint16_t mv) { if (mv % 16) return false; vreg_mv = mv; return true; }
  void setTurnOffDelay(uint8_t on) { if (on) reg[0x09] |= 0x08; else reg[0x09] &= (uint8_t)~0x08; }
  void shutdown() { ++shutdown_calls; reg[0x09] |= 0x20; }
};
