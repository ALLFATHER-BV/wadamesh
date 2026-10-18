// SPDX-License-Identifier: GPL-3.0-or-later
#include "IndicatorBoard.h"

#include <Arduino.h>

#include "IndicatorIo.h"

void IndicatorBoard::begin() {
  ESP32Board::begin();   // starts Wire on PIN_BOARD_SDA/PIN_BOARD_SCL
  if (!IndicatorIo::begin()) Serial.println("[indicator] expander unavailable");
}
