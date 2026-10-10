// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RadioLib HAL for the SenseCAP Indicator. SCK/MISO/MOSI are real GPIOs on the
// FSPI peripheral, but the SX1262's NSS, RESET, BUSY and DIO1 sit on the TCA9535
// (P0.0..P0.3). The env names them with virtual pins 0x40..0x43; this HAL turns
// those into expander transfers and hands every real GPIO straight to the
// Arduino pin functions, as ArduinoHal does.
//
// DIO1 has no GPIO edge. The expander's open-drain /INT on GPIO42 wakes a
// dispatch task that reads port 0, and every port-0 read -- that one, or a BUSY
// poll RadioLib makes -- goes through Dio1Edge, which calls the radio callback
// once per DIO1 rise. TX-done and RX-done both arrive that way, so the stock
// core sees the same callback an IRQ board does.

#include <RadioLib.h>
#include <SPI.h>

#include "IndicatorDio1.h"

class IndicatorRadioHal : public ArduinoHal {
public:
  explicit IndicatorRadioHal(SPIClass& spi, uint32_t spiFreq = 4000000);

  void init() override;
  void pinMode(uint32_t pin, uint32_t mode) override;
  void digitalWrite(uint32_t pin, uint32_t value) override;
  uint32_t digitalRead(uint32_t pin) override;
  void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
  void detachInterrupt(uint32_t interruptNum) override;
  uint32_t pinToInterrupt(uint32_t pin) override;

  // Creates the dispatch task and the GPIO42 ISR. Idempotent; attachInterrupt on
  // DIO1 calls it the first time.
  void startDispatch();

private:
  static void onPort0(uint8_t value, void* ctx);

  indicator::Dio1Edge _dio1;
};
