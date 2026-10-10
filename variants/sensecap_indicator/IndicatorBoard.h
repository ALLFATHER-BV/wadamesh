// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ESP32Board.h>

class IndicatorBoard : public ESP32Board {
public:
  void begin();
  uint16_t getBattMilliVolts() override { return 0; }   // mains-powered, no battery
  bool isExternalPowered() override { return true; }
  const char* getManufacturerName() const override { return "Seeed SenseCAP Indicator"; }
};
