// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

TWatchS3Board board;

// LoRa radio on FSPI (the Arduino default SPI: SCK=3/MISO=4/MOSI=1/NSS=5).
// The ST7789 display sits on a SEPARATE SPI3_HOST bus (see LGFXDisplay.cpp),
// so there is zero contention between the two.
#if defined(P_LORA_SCLK)
  static SPIClass spi(FSPI);
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  MicroNMEALocationProvider gps(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors(gps);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

  // Power-up the TFT backlight so the panel controller is live before
  // LGFXDisplay::begin() probes it.
#if PIN_TFT_LEDA_CTL >= 0
  pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
  digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
  delay(50);
#endif

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
