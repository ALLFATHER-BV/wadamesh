// SPDX-License-Identifier: GPL-3.0-or-later
// Fake SensorLib DRV2605: the same register writes as SensorLib 0.3.3's
// SensorDRV2605.hpp (WAVESEQ1 0x04 + slot, LIBRARY 0x03, GO 0x0C, MODE 0x01) into a
// register file a test reads. GO stays 1 until the test ends the "playback".
#pragma once
#include <cstdint>
#include "Wire.h"
struct FakeDrvComm {
  uint8_t reg[0x23] = {};
  int writes = 0;
  void writeRegister(uint8_t r, uint8_t v) { reg[r] = v; ++writes; }
  int readRegister(uint8_t r) { return reg[r]; }
};
extern FakeDrvComm g_drv;
extern bool g_drv_present;
class SensorDRV2605 {
protected:
  FakeDrvComm* comm = nullptr;
public:
  static constexpr uint8_t MODE_INTTRIG = 0x00;
  bool begin(TwoWire&) { if (!g_drv_present) return false; comm = &g_drv; return true; }
  void setWaveform(uint8_t slot, uint8_t w) { comm->writeRegister(0x04 + slot, w); }
  void selectLibrary(uint8_t lib) { comm->writeRegister(0x03, lib); }
  void run() { comm->writeRegister(0x0C, 1); }
  void stop() { comm->writeRegister(0x0C, 0); }
  void setMode(uint8_t m) { comm->writeRegister(0x01, m); }
};
