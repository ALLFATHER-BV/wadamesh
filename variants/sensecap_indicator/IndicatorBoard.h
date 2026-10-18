// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ESP32Board.h>

#include "IndicatorExpander.h"

class IndicatorBoard : public ESP32Board {
public:
  void begin();
  uint16_t getBattMilliVolts() override { return 0; }   // mains-powered, no battery
  bool isExternalPowered() override { return true; }
  const char* getManufacturerName() const override { return "Seeed SenseCAP Indicator"; }

  // P_LORA_DIO_1 is an expander pin, not a GPIO. The line that reports radio
  // events to the ESP32 is the expander's /INT on GPIO42 (active low).
  uint32_t getIRQGpio() override { return indicator::kExpanderIntGpio; }

  // ESP32Board::sleep light-sleeps with getIRQGpio() as a high-level wake source.
  // /INT is active low and DIO1 is only seen through I2C, so never light-sleep.
  void sleep(uint32_t secs) override { (void)secs; delay(1); }
};
