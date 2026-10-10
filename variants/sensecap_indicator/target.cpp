// SPDX-License-Identifier: GPL-3.0-or-later
#include <Arduino.h>

#include "target.h"
#include "IndicatorIo.h"
#include "IndicatorRadioHal.h"

IndicatorBoard board;

// The radio's NSS/RESET/BUSY/DIO1 sit on the TCA9535; the P_LORA_* values are
// virtual pins that IndicatorRadioHal resolves to expander bits. SCK/MISO/MOSI
// are real GPIOs on FSPI.
static SPIClass radioSpi(FSPI);
static IndicatorRadioHal radio_hal(radioSpi);
RADIO_CLASS radio = new Module(&radio_hal, P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC rtc_clock(fallback_clock);

EnvironmentSensorManager sensors;

IndicatorDisplay display;
MomentaryButton user_btn(PIN_USER_BTN, 1000, true);

// The strap picks the TCXO setting. std_init only falls back from TCXO to 0 V,
// so a TCXO board whose strap misreads as crystal would never start: a failed
// start tries the other setting before giving up. main.cpp's retry after a
// transient failure runs both again.
static bool s_tcxoAlternate = false;

float indicatorTcxoVoltage() {
  const bool tcxo = IndicatorIo::radioHasTcxo() != s_tcxoAlternate;
  return tcxo ? 2.4f : 0.0f;
}

static bool radioStart(bool alternate) {
  s_tcxoAlternate = alternate;
  Serial.printf("[indicator] radio tcxo requested=%.1f V (strap: %s%s)\n", indicatorTcxoVoltage(),
                IndicatorIo::radioHasTcxo() ? "tcxo" : "crystal",
                alternate ? ", trying the other setting" : "");
  // std_init starts FSPI on GPIO41/47/48; main.cpp runs display.begin() first,
  // and the panel init has handed GPIO41/48 back by then.
  const bool ok = radio.std_init(&radioSpi);
  // std_init and RadioLib can each drop to 0 V on their own; tcxoVoltage holds
  // the value the last begin() ran with.
  Serial.printf("[indicator] radio %s, tcxo used=%.1f V\n", ok ? "up" : "init failed",
                radio.tcxoVoltage);
  return ok;
}

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  return radioStart(false) || radioStart(true);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
