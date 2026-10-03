// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

CrowPanel35Board board;
static SPIClass radioSpi(FSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, radioSpi);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);
#else
EnvironmentSensorManager sensors;
#endif

CrowPanel35Display display;

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  if (!radio.std_init(&radioSpi)) {
    Serial.println("[crowpanel-35] SX1262 not found on the LoRa expansion slot");
    return false;
  }
  return true;
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
