// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

CrowPanel50Board board;
static SPIClass radioSpi(FSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, radioSpi);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

// No GPS: every GPIO a UART could use is taken by the RGB panel, the radio or
// the touch bus (an unpinned Serial1 would fall back to GPIO15/16, the GT911).
EnvironmentSensorManager sensors;

CrowPanel50Display display;

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  if (!radio.std_init(&radioSpi)) {
    // The function switches S1/S0 on the back share the slot's pins with the SD card
    // and the speaker/microphone; only WM (S1 0, S0 1) connects the radio. Elecrow
    // sells the board with and without a module, and the slot also takes non-LoRa ones.
    Serial.printf("[crowpanel-50] SX1262 not found (CS%d). Fit an SX1262 module in J8/J9 and set "
                  "the function switches S1/S0 to WM (0 1).\n", P_LORA_NSS);
    return false;
  }
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
