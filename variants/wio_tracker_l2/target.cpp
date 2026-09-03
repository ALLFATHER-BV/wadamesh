// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>

#include "target.h"

WioTrackerL2Board board;

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

WioTrackerL2Display display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  return radio.std_init(&radioSpi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}