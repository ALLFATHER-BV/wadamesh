// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>
#include "target.h"

RAKTapV2Board board;

// LoRa radio on HSPI (SPI3_HOST on S3). The TFT display uses pin-based
// software SPI (USE_PIN_TFT), so there is zero bus contention — the two
// peripherals are on completely independent buses.
#if defined(P_LORA_SCLK)
  static SPIClass spi(HSPI);
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

SPIClass* rakTapV2SharedSPI() {
#if defined(P_LORA_SCLK)
  return &spi;
#else
  return nullptr;
#endif
}

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  MicroNMEALocationProvider gps(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors(gps);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display(&board.periph_power);
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

  // Power-up the TFT backlight so the panel controller is live before
  // ST7789LCDDisplay::begin() probes it.
#if PIN_TFT_LEDA_CTL >= 0
  pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
  digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
  delay(50);
#endif

  // Start Arduino default SPI (FSPI=SPI2_HOST) on the TFT GPIOs so the
  // display driver (LovyanGFX with bus_shared=true) finds an already-begun
  // bus and skips spi_bus_initialize — avoiding a hang from double-init.
  SPI.begin(PIN_TFT_SCL, 10, PIN_TFT_SDA, PIN_TFT_CS);

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
