// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>

// Elecrow CrowPanel Advance 5.0" (ESP32-S3-WROOM-1-N16R8). The expansion slot
// is routed to the SX1262 by the DIP switch on the back of the board, not by a
// GPIO, so there is nothing to set up here beyond the shared I2C bus that
// ESP32Board::begin() opens on GPIO15/16 (GT911 + backlight controller).
class CrowPanel50Board : public ESP32Board {
public:
  void begin() {
    ESP32Board::begin();
  }

  const char* getManufacturerName() const override { return "Elecrow CrowPanel Advance 5.0"; }
};
