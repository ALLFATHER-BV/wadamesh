// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>

#include "target.h"
#include "IndicatorIo.h"

IndicatorBoard board;

// The radio's NSS/RESET/BUSY/DIO1 sit on the TCA9535; the P_LORA_* values are
// virtual pins that a HAL resolves to expander bits. Until that HAL exists this
// is a plain Module, which links but cannot drive the radio.
static SPIClass radioSpi(FSPI);
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, radioSpi);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

EnvironmentSensorManager sensors;

IndicatorDisplay display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);

float indicatorTcxoVoltage() {
  return IndicatorIo::radioHasTcxo() ? 2.4f : 0.0f;
}

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  return radio.std_init(&radioSpi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
