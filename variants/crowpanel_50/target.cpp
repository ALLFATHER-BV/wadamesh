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
    // The DIP switch on the back shares the slot's pins with the SD card and the
    // speaker/microphone; only its wireless-module position connects the radio.
    Serial.printf("[crowpanel-50] SX1262 not found (CS%d). Set the function DIP switch to the "
                  "wireless module and seat the module in J8/J9.\n", P_LORA_NSS);
    return false;
  }
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
