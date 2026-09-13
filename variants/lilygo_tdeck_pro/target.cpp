// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

TDeckProBoard board;
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);
WadaNmeaLocationProvider gps(Serial1, &rtc_clock, GPS_RESET, GPS_EN,
                             PIN_GPS_TX, PIN_GPS_RX, GPS_BAUD_RATE);
EnvironmentSensorManager sensors(gps);
TDeckProDisplay display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);

SPIClass* tdeckSharedSPI() { return &SPI; }

bool radio_init() {
  fallback_clock.begin();
  return radio.std_init(&SPI);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}