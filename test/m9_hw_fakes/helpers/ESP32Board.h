// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

class ESP32Board {
public:
  virtual ~ESP32Board() {}
  void begin() {}
  virtual void powerOff() {}
  virtual uint16_t getBattMilliVolts() { return 0; }
  virtual bool isExternalPowered() { return false; }
  virtual const char* getManufacturerName() const { return ""; }
};
