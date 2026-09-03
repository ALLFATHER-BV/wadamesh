// SPDX-License-Identifier: GPL-3.0-or-later
#include "WioTrackerL2Board.h"

#include <Arduino.h>

#include "WioTrackerL2Io.h"

void WioTrackerL2Board::begin() {
  ESP32Board::begin();
  Wire.setClock(100000);
  if (!WioTrackerL2Io::begin()) Serial.println("[wio-l2] expander unavailable");
}

void WioTrackerL2Board::powerOff() {
  (void)WioTrackerL2Io::setAudioPaPower(false);
  (void)WioTrackerL2Io::setSdPower(false);
  (void)WioTrackerL2Io::setGnssPower(false);
  (void)WioTrackerL2Io::setLcdPower(false);
  enterDeepSleep(0);
}

bool WioTrackerL2Board::ensureBatteryAdc() {
  if (_batteryAdcReady) return true;
  const uint32_t now = millis();
  if (_nextBatteryProbeMs && (int32_t)(now - _nextBatteryProbeMs) < 0) return false;
  if (!WioTrackerL2Io::ready() || !WioTrackerL2Io::setBatterySense(true)) return false;
  delay(10);
  _batteryAdcReady = _batteryAdc.begin(0x48, &Wire);
  if (_batteryAdcReady) _batteryAdc.setGain(GAIN_TWO);
  (void)WioTrackerL2Io::setBatterySense(false);
  if (!_batteryAdcReady) _nextBatteryProbeMs = now + 15000;
  return _batteryAdcReady;
}

// Adafruit's readADC_SingleEnded() ends in `while (!conversionComplete());` —
// an UNBOUNDED busy-wait. conversionComplete() is itself an I2C read, so if the
// ADS1115 stops ACKing, that loop never returns. It runs on loopTask, under the
// status-bar refresh, so a silent ADC took the whole UI down with it: measured
// [STALL] ui:status of 3 s, 38 s and 94 s on bring-up hardware. Bounded here
// instead. The chip converts in ~8 ms at the default 128 SPS; 50 ms is slack,
// and the delay(1) yields to IDLE so the task watchdog stays fed.
int16_t WioTrackerL2Board::readBatteryAdcBounded() {
  _batteryAdc.startADCReading(ADS1X15_REG_CONFIG_MUX_SINGLE_0, /*continuous=*/false);
  const uint32_t deadline = millis() + 50;
  while (!_batteryAdc.conversionComplete()) {
    if ((int32_t)(millis() - deadline) >= 0) return -1;   // ADC went quiet
    delay(1);
  }
  return _batteryAdc.getLastConversionResults();
}

uint16_t WioTrackerL2Board::getBattMilliVolts() {
  const uint32_t now = millis();
  if (_lastBatteryMv && (uint32_t)(now - _lastBatteryReadMs) < 30000) return _lastBatteryMv;
  if (!ensureBatteryAdc() || !WioTrackerL2Io::setBatterySense(true)) return _lastBatteryMv;
  delay(10);
  float volts = 0.0f;
  uint8_t samples = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t raw = readBatteryAdcBounded();
    if (raw > 0) { volts += _batteryAdc.computeVolts(raw); ++samples; }
  }
  (void)WioTrackerL2Io::setBatterySense(false);
  if (samples) {
    _lastBatteryMv = (uint16_t)((volts / samples) * 2000.0f);
    _lastBatteryReadMs = now;
  } else {
    // A failed read used to leave BOTH _lastBatteryMv and _lastBatteryReadMs
    // untouched, so the 30 s cache above never armed (it is gated on a NON-ZERO
    // reading) and every single status tick retried the full three-sample read.
    // That is why the stalls grew instead of staying isolated. Drop back to the
    // probe path, which ensureBatteryAdc() rate-limits to one attempt per 15 s.
    _batteryAdcReady   = false;
    _nextBatteryProbeMs = now + 15000;
  }
  return _lastBatteryMv;
}

bool WioTrackerL2Board::sdCardPresent() {
  bool present = false;
  return WioTrackerL2Io::readSdPresent(present) && present;
}