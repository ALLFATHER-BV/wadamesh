// SPDX-License-Identifier: GPL-3.0-or-later
#include "IndicatorIo.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {

constexpr uint8_t kAddress = 0x20;

// Created on first use; the compiler guards the local static's initialisation,
// so two tasks that reach the first BusLock together still get one mutex.
SemaphoreHandle_t busMutex() {
  static StaticSemaphore_t storage;
  static const SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutexStatic(&storage);
  return handle;
}

class WireBus : public indicator::ExpanderBus {
public:
  bool read(uint8_t reg, uint8_t* data, size_t n) override {
    IndicatorIo::BusLock lock;
    Wire.beginTransmission(kAddress);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)kAddress, (int)n) != n) return false;
    for (size_t i = 0; i < n; ++i) data[i] = (uint8_t)Wire.read();
    return true;
  }

  bool write(uint8_t reg, const uint8_t* data, size_t n) override {
    IndicatorIo::BusLock lock;
    Wire.beginTransmission(kAddress);
    Wire.write(reg);
    for (size_t i = 0; i < n; ++i) Wire.write(data[i]);
    return Wire.endTransmission() == 0;
  }
};

WireBus s_bus;
indicator::Tca9535 s_chip(s_bus);
bool s_ready = false;
bool s_tcxo = false;

}  // namespace

namespace IndicatorIo {

BusLock::BusLock() { xSemaphoreTakeRecursive(busMutex(), portMAX_DELAY); }
BusLock::~BusLock() { xSemaphoreGiveRecursive(busMutex()); }

bool begin() {
  using namespace indicator::pins;
  BusLock lock;
  s_ready = false;
  Wire.setClock(400000);

  if (!s_chip.begin()) {
    Serial.println("[indicator] expander: seed read failed (no ack at 0x20?)");
    return false;
  }
  // What the previous firmware or a warm reset left behind.
  Serial.printf("[indicator] expander seeded out=%02X/%02X cfg=%02X/%02X\n", s_chip.output(0),
                s_chip.output(1), s_chip.config(0), s_chip.config(1));

  // NSS high first, so the SX1262 ignores the panel-init bytes on the shared lines.
  if (!s_chip.setOutput(kLoraNss, true) || !s_chip.setOutput(kLcdCs, true) ||
      !s_chip.setOutput(kLcdReset, true) || !s_chip.setOutput(kTouchReset, true) ||
      !s_chip.setOutput(kRp2040Reset, true)) {
    Serial.println("[indicator] expander: output config failed");
    return false;
  }
  if (!s_chip.setInput(kLoraBusy) || !s_chip.setInput(kLoraDio1) ||
      !s_chip.setInput(kTouchInt) || !s_chip.setInput(kRadioStrap)) {
    Serial.println("[indicator] expander: input config failed");
    return false;
  }

  bool samples[5];
  for (int i = 0; i < 5; ++i) {
    if (!s_chip.readPin(kRadioStrap, samples[i])) {
      Serial.println("[indicator] expander: strap read failed");
      return false;
    }
    delay(1);
  }
  s_tcxo = indicator::strapMajority(samples);

  s_ready = true;
  Serial.printf("[indicator] expander out=%02X/%02X cfg=%02X/%02X tcxo=%d\n",
                s_chip.output(0), s_chip.output(1), s_chip.config(0), s_chip.config(1),
                s_tcxo ? 1 : 0);
  return true;
}

bool ready() { return s_ready; }

indicator::Tca9535& chip() { return s_chip; }

bool radioHasTcxo() { return s_tcxo; }

}  // namespace IndicatorIo
