// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Adafruit_ADS1X15.h>
#include <helpers/ESP32Board.h>

class WioTrackerL2Board : public ESP32Board {
public:
  void begin();
  void powerOff() override;
  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override { return "Seeed Wio Tracker L2"; }
  bool sdCardPresent();

private:
  bool ensureBatteryAdc();
  int16_t readBatteryAdcBounded();   // timeout-bounded single-ended read (see .cpp)

  Adafruit_ADS1115 _batteryAdc;
  bool _batteryAdcReady = false;
  uint16_t _lastBatteryMv = 0;
  uint32_t _lastBatteryReadMs = 0;
  uint32_t _nextBatteryProbeMs = 0;
};