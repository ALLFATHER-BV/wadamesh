// SPDX-License-Identifier: GPL-3.0-or-later
#include "WioTrackerL2Io.h"

#include <Arduino.h>
#include <Wire.h>

namespace {

constexpr uint8_t kAddress = 0x21;
constexpr uint8_t kRegInput0 = 0x00;
constexpr uint8_t kRegOutput0 = 0x02;
constexpr uint8_t kRegConfig0 = 0x06;

constexpr uint8_t kWakeButton = 0;
constexpr uint8_t kI2cInterrupt = 1;
constexpr uint8_t kSdDetect = 2;
constexpr uint8_t kTouchInterrupt = 3;
constexpr uint8_t kLcdControl = 4;
constexpr uint8_t kLcdPower = 5;
constexpr uint8_t kLcdReset = 6;
constexpr uint8_t kGrovePower = 7;
constexpr uint8_t kTouchReset = 8;
constexpr uint8_t kGnssReset = 9;
constexpr uint8_t kUserLed = 10;
constexpr uint8_t kUsbOtg = 11;
constexpr uint8_t kAudioPa = 12;
constexpr uint8_t kGnssPower = 13;
constexpr uint8_t kSdPower = 14;
constexpr uint8_t kBatterySense = 15;

uint8_t s_output[2] = {0xFF, 0xFF};
uint8_t s_config[2] = {0xFF, 0xFF};
bool s_ready = false;

bool readRegisters(uint8_t reg, uint8_t* data, size_t length) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kAddress, (int)length) != length) return false;
  for (size_t i = 0; i < length; ++i) data[i] = (uint8_t)Wire.read();
  return true;
}

bool writeRegisters(uint8_t reg, const uint8_t* data, size_t length) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  for (size_t i = 0; i < length; ++i) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

void stageInput(uint8_t bit) {
  s_config[bit >> 3] |= (uint8_t)(1U << (bit & 7));
}

void stageOutput(uint8_t bit, bool high) {
  const uint8_t mask = (uint8_t)(1U << (bit & 7));
  uint8_t& output = s_output[bit >> 3];
  uint8_t& config = s_config[bit >> 3];
  if (high) output |= mask;
  else output &= (uint8_t)~mask;
  config &= (uint8_t)~mask;
}

bool writeState() {
  // Latch output levels before exposing pins as outputs.
  return writeRegisters(kRegOutput0, s_output, sizeof(s_output)) &&
         writeRegisters(kRegConfig0, s_config, sizeof(s_config));
}

bool setOutput(uint8_t bit, bool high) {
  if (!s_ready) return false;
  const uint8_t output0 = s_output[0], output1 = s_output[1];
  const uint8_t config0 = s_config[0], config1 = s_config[1];
  stageOutput(bit, high);
  if (writeState()) return true;
  s_output[0] = output0; s_output[1] = output1;
  s_config[0] = config0; s_config[1] = config1;
  return false;
}

bool readInputBit(uint8_t bit, bool& high) {
  uint8_t inputs[2] = {0xFF, 0xFF};
  if (!s_ready || !readRegisters(kRegInput0, inputs, sizeof(inputs))) return false;
  high = (inputs[bit >> 3] & (uint8_t)(1U << (bit & 7))) != 0;
  return true;
}

}  // namespace

namespace WioTrackerL2Io {

bool begin() {
  s_ready = false;
  Wire.setClock(100000);
  Wire.setTimeOut(30);
  pinMode(45, INPUT_PULLUP);

  if (!readRegisters(kRegOutput0, s_output, sizeof(s_output)) ||
      !readRegisters(kRegConfig0, s_config, sizeof(s_config))) {
    Serial.println("[wio-l2] expander probe failed");
    return false;
  }

  stageInput(kWakeButton);
  stageInput(kI2cInterrupt);
  stageInput(kSdDetect);
  stageInput(kLcdControl);
  stageOutput(kTouchInterrupt, false);
  stageOutput(kLcdPower, true);
  stageOutput(kLcdReset, true);
  stageOutput(kGrovePower, false);
  stageOutput(kTouchReset, false);
  stageOutput(kGnssReset, true);
  stageOutput(kUserLed, false);
  stageOutput(kUsbOtg, false);
  stageOutput(kAudioPa, false);
  stageOutput(kGnssPower, true);
  stageOutput(kSdPower, false);
  stageOutput(kBatterySense, false);
  if (!writeState()) return false;

  delay(10);
  stageOutput(kGnssReset, false);
  if (!writeState()) return false;
  delay(40);
  stageOutput(kLcdReset, false);
  if (!writeState()) return false;
  delay(10);
  stageOutput(kLcdReset, true);
  if (!writeState()) return false;
  delay(500);
  stageOutput(kLcdControl, true);
  if (!writeState()) return false;
  delay(10);
  stageOutput(kTouchReset, true);
  if (!writeState()) return false;
  delay(60);

  s_ready = true;
  Serial.printf("[wio-l2] expander ready out=%02X/%02X cfg=%02X/%02X\n",
                s_output[0], s_output[1], s_config[0], s_config[1]);
  return true;
}

bool ready() { return s_ready; }

bool readWakeButton(bool& pressed) {
  bool high = true;
  if (!readInputBit(kWakeButton, high)) return false;
  pressed = !high;
  return true;
}

bool readSdPresent(bool& present) {
  bool high = true;
  if (!readInputBit(kSdDetect, high)) return false;
  present = !high;
  return true;
}

bool setLcdPower(bool enabled) { return setOutput(kLcdPower, enabled); }
bool setLcdResetReleased(bool released) { return setOutput(kLcdReset, released); }
bool setTouchResetReleased(bool released) { return setOutput(kTouchReset, released); }
bool setGnssPower(bool enabled) { return setOutput(kGnssPower, enabled); }
bool setGnssResetReleased(bool released) { return setOutput(kGnssReset, !released); }
bool setSdPower(bool enabled) { return setOutput(kSdPower, enabled); }
bool setBatterySense(bool enabled) { return setOutput(kBatterySense, enabled); }
bool setAudioPaPower(bool enabled) { return setOutput(kAudioPa, enabled); }

}  // namespace WioTrackerL2Io