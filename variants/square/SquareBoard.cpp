// SPDX-License-Identifier: GPL-3.0-or-later
#include "SquareBoard.h"

#include <Arduino.h>

#include "SquareIo.h"

void SquareBoard::begin() {
  ESP32Board::begin();
  Wire.setClock(100000);
  if (!SquareIo::begin()) Serial.println("[square] expander unavailable");
}

void SquareBoard::powerOff() {
  (void)SquareIo::setAudioPaPower(false);
  (void)SquareIo::setSdPower(false);
  (void)SquareIo::setGnssPower(false);
  (void)SquareIo::setLcdPower(false);
  enterDeepSleep(0);
}

bool SquareBoard::ensureBatteryAdc() {
  if (_batteryAdcReady) return true;
  const uint32_t now = millis();
  if (_nextBatteryProbeMs && (int32_t)(now - _nextBatteryProbeMs) < 0) return false;
  if (!SquareIo::ready() || !SquareIo::setBatterySense(true)) return false;
  delay(10);
  _batteryAdcReady = _batteryAdc.begin(0x48, &Wire);
  if (_batteryAdcReady) _batteryAdc.setGain(GAIN_TWO);
  (void)SquareIo::setBatterySense(false);
  if (!_batteryAdcReady) _nextBatteryProbeMs = now + 15000;
  return _batteryAdcReady;
}

uint16_t SquareBoard::getBattMilliVolts() {
  const uint32_t now = millis();
  if (_lastBatteryMv && (uint32_t)(now - _lastBatteryReadMs) < 30000) return _lastBatteryMv;
  if (!ensureBatteryAdc() || !SquareIo::setBatterySense(true)) return _lastBatteryMv;
  delay(10);
  float volts = 0.0f;
  uint8_t samples = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t raw = _batteryAdc.readADC_SingleEnded(0);
    if (raw > 0) { volts += _batteryAdc.computeVolts(raw); ++samples; }
  }
  (void)SquareIo::setBatterySense(false);
  if (samples) {
    _lastBatteryMv = (uint16_t)((volts / samples) * 2000.0f);
    _lastBatteryReadMs = now;
  }
  return _lastBatteryMv;
}

bool SquareBoard::sdCardPresent() {
  bool present = false;
  return SquareIo::readSdPresent(present) && present;
}