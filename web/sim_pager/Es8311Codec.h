// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// T-LoRa Pager simulator: the ES8311 codec, absent.
#include <stdint.h>
class TwoWire;
class Es8311Codec {
public:
  bool begin(TwoWire&, uint8_t = 0x18) { return true; }
  bool start(uint32_t) { return true; }
  void suspend() {}
  void setMute(bool) {}
  void setVolumePercent(uint8_t) {}
  bool writeReg(uint8_t, uint8_t) { return true; }
  bool readReg(uint8_t, uint8_t* v) { if (v) *v = 0; return true; }
};
