// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>

class CrowPanel35Board : public ESP32Board {
public:
  void begin() {
    digitalWrite(PIN_SD_CS, HIGH);
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(45, LOW);  // LOW routes the expansion slot to LoRa, not the microphone.
    pinMode(45, OUTPUT);
    ESP32Board::begin();
  }

  const char* getManufacturerName() const override { return "Elecrow CrowPanel Advance 3.5"; }
};
